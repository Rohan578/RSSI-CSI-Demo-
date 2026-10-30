/*
 * ============================================================================
 *  CSI RECEIVER  -  ESP32-S3, Arduino IDE version
 * ============================================================================
 *
 *  WHAT THIS BOARD DOES
 *    1. Joins the WiFi access point (so it can talk to the Raspberry Pi).
 *    2. Listens for the ESP-NOW packets the transmitters broadcast.
 *    3. For every packet from a transmitter, the WiFi hardware measures
 *       CSI: how each subcarrier (small slice of the radio channel) was
 *       weakened and shifted on its way across the room. People in the
 *       room change these numbers - that's what the AI model learns from.
 *    4. Packs the CSI samples into UDP packets and sends them to the Pi.
 *
 *  DATA FLOW INSIDE THIS SKETCH
 *
 *     WiFi radio ──> onCsiReceived()  ──> queue ──> loop() ──> UDP ──> Pi
 *                    (runs inside the               (batches samples,
 *                     WiFi driver: must              sends them, prints
 *                     be FAST, only copies)          stats)
 *
 *  BASED ON
 *    Espressif's official "csi_recv" example
 *    (github.com/espressif/esp-csi, examples/get-started/csi_recv).
 *    Changes: listens to many transmitters (by MAC), stays connected to an
 *    access point, sends CSI over UDP instead of printing it, and keeps
 *    per-node counters.
 *
 *  ARDUINO IDE SETUP
 *    - Boards Manager: "esp32 by Espressif Systems", version 3.3.0 or newer
 *    - Tools > Board:  "ESP32S3 Dev Module"  (or your exact S3 board)
 *    - Tools > USB CDC On Boot:
 *        "Enabled"  if you plug into the S3's native USB port (often "USB")
 *        "Disabled" if you plug into the USB-to-serial port (often "UART"/"COM")
 *    - Serial Monitor: 115200 baud
 *
 *  FILES (tabs at the top of the Arduino IDE)
 *    config.h          <- WiFi, Pi IP address, LED. EDIT THIS.
 *    csi_packet.h      <- UDP format sent to the Pi (server.py must match)
 *    espnow_packet.h   <- what transmitters send (must match transmitter)
 *    csi_receiver_s3   <- this file
 *
 *  SECTIONS IN THIS FILE
 *    1. Includes and shared state
 *    2. Status LED
 *    3. WiFi connection
 *    4. Reading the transmitter's packet counter
 *    5. CSI callback (the fast part)
 *    6. ESP-NOW callback (just counts packets heard)
 *    7. CSI + ESP-NOW setup
 *    8. UDP batching and sending
 *    9. Stats printer
 *   10. setup()  - runs once
 *   11. loop()   - runs forever
 * ============================================================================
 */


/* ============================================================================
 *  SECTION 1: INCLUDES AND SHARED STATE
 * ========================================================================== */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_timer.h>

#include "config.h"
#include "csi_packet.h"
#include "espnow_packet.h"

// One CSI sample as it travels from the WiFi callback to loop()
typedef struct {
  csi_sample_header_t header;
  int8_t              data[CSI_MAX_LEN];
} CsiQueueItem;

static QueueHandle_t csiQueue;                  // Callback -> loop()
static const uint8_t nodePrefix[5] = NODE_MAC_PREFIX;

WiFiUDP   udp;
IPAddress piAddress;

// Per-transmitter counters (index = node ID). Updated by the WiFi driver,
// read by the stats printer.
static volatile uint32_t nodeCsiCount[256];     // CSI samples captured
static volatile uint32_t nodeHeardCount[256];   // ESP-NOW packets heard
static volatile int8_t   nodeLastRssi[256];     // Most recent signal strength
static volatile uint16_t nodeCsiLen[256];       // CSI size from this node

// Overall counters
static volatile uint32_t totalCsi       = 0;    // CSI samples captured
static volatile uint32_t queueFullDrops = 0;    // Lost: queue was full
static uint32_t udpPacketsSent = 0;             // UDP packets to the Pi
static uint32_t samplesSent    = 0;             // CSI samples inside them
static uint32_t udpErrors      = 0;             // UDP sends that failed


/* ============================================================================
 *  SECTION 2: STATUS LED
 *    off       = not connected to WiFi
 *    solid on  = connected, but no CSI arriving yet
 *    blinking  = connected and CSI is flowing (blinks once per second)
 * ========================================================================== */
#if STATUS_LED_GPIO == -2
  #ifdef LED_BUILTIN
    static const int ledPin = LED_BUILTIN;   // Board's own LED
  #else
    static const int ledPin = -1;            // This board has none
  #endif
#else
  static const int ledPin = STATUS_LED_GPIO;
#endif

void ledInit() {
  if (ledPin < 0) return;
  pinMode(ledPin, OUTPUT);
}

void ledSet(bool on) {
  if (ledPin < 0) return;
  bool level = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(ledPin, level ? HIGH : LOW);
}


/* ============================================================================
 *  SECTION 3: WIFI CONNECTION
 *  Joins the access point. The Arduino WiFi library reconnects by itself
 *  if the connection drops.
 * ========================================================================== */
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // No power saving: never miss a packet
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] Connecting to \"%s\"", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[WIFI] Still not connected. Check WIFI_SSID / WIFI_PASSWORD");
      Serial.println("[WIFI] and that the access point is 2.4 GHz. Retrying...");
      start = millis();
    }
  }

  Serial.println();
  Serial.printf("[WIFI] Connected. S3 IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Signal from access point: %d dBm\n", WiFi.RSSI());
  Serial.println("--------------------------------------------------");
  Serial.printf ("  ACCESS POINT CHANNEL: %d\n", (int)WiFi.channel());
  Serial.println("  Set WIFI_CHANNEL to this number on EVERY transmitter.");
  Serial.println("--------------------------------------------------");
}


/* ============================================================================
 *  SECTION 4: READING THE TRANSMITTER'S PACKET COUNTER
 *  The CSI callback also gives us the raw bytes of the received frame.
 *  Our 12-byte tx_packet_t sits somewhere inside the ESP-NOW frame, after
 *  ESP-NOW's own header. We search the first part of the frame for our
 *  magic number + version + node ID, then read the counter (seq).
 *  Returns 0 if not found - the CSI is still used, just without a counter.
 * ========================================================================== */
uint32_t findTxSeq(const uint8_t *payload, uint16_t payloadLen, uint8_t nodeId) {
  if (payload == nullptr || payloadLen < sizeof(tx_packet_t)) return 0;

  const uint8_t magicLow  = TX_PACKET_MAGIC & 0xFF;
  const uint8_t magicHigh = TX_PACKET_MAGIC >> 8;
  uint16_t searchEnd = payloadLen - sizeof(tx_packet_t);
  if (searchEnd > 64) searchEnd = 64;           // It's always near the start

  for (uint16_t i = 0; i <= searchEnd; i++) {
    if (payload[i] == magicLow && payload[i + 1] == magicHigh &&
        payload[i + 2] == TX_PACKET_VERSION && payload[i + 3] == nodeId) {
      tx_packet_t packet;
      memcpy(&packet, payload + i, sizeof(packet));
      return packet.seq;
    }
  }
  return 0;
}


/* ============================================================================
 *  SECTION 5: CSI CALLBACK  (the fast part)
 *  The WiFi driver calls this for EVERY packet it receives, from every
 *  device nearby. It runs inside the WiFi driver, so it must be quick:
 *  filter, copy into the queue, return. Never print or wait here.
 * ========================================================================== */
void onCsiReceived(void *ctx, wifi_csi_info_t *info) {
  if (info == nullptr || info->buf == nullptr) return;

  // Keep only packets from our transmitters: MAC 1a:00:00:00:00:<node>
  if (memcmp(info->mac, nodePrefix, sizeof(nodePrefix)) != 0) return;
  uint8_t nodeId = info->mac[5];
  if (nodeId == 0 || nodeId == 255) return;

  // "static" keeps this large buffer off the WiFi task's small stack.
  // Safe because the driver only ever calls this from one task.
  static CsiQueueItem item;
  const wifi_pkt_rx_ctrl_t *rx = &info->rx_ctrl;

  item.header.tx_node_id         = nodeId;
  item.header.rssi               = (int8_t)rx->rssi;
  item.header.noise_floor        = (int8_t)rx->noise_floor;
  item.header.channel            = (uint8_t)rx->channel;
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32C61
  item.header.rx_format          = (uint8_t)rx->cur_bb_format;
#else
  item.header.rx_format          = (uint8_t)rx->sig_mode;
#endif
  item.header.first_word_invalid = info->first_word_invalid ? 1 : 0;
  item.header.tx_seq             = findTxSeq(info->payload, info->payload_len, nodeId);
  item.header.rx_timestamp_us    = rx->timestamp;

  uint16_t len = info->len;
  if (len > CSI_MAX_LEN) len = CSI_MAX_LEN;
  item.header.csi_len = len;
  memcpy(item.data, info->buf, len);

  // Update per-node counters for the stats printer
  nodeCsiCount[nodeId] = nodeCsiCount[nodeId] + 1;
  nodeLastRssi[nodeId] = item.header.rssi;
  nodeCsiLen[nodeId]   = len;
  totalCsi = totalCsi + 1;

  // Never wait here: if loop() has fallen behind, drop and count it
  if (xQueueSend(csiQueue, &item, 0) != pdTRUE) {
    queueFullDrops = queueFullDrops + 1;
  }
}


/* ============================================================================
 *  SECTION 6: ESP-NOW CALLBACK  (just counts packets heard)
 *  Not needed for CSI, but very useful for debugging: if a node shows up
 *  here but has no CSI, it's a CSI setting; if it doesn't show up at all,
 *  it's the channel, distance or the transmitter itself.
 * ========================================================================== */
void onEspNowReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (info == nullptr || info->src_addr == nullptr) return;
  if (memcmp(info->src_addr, nodePrefix, sizeof(nodePrefix)) != 0) return;
  uint8_t nodeId = info->src_addr[5];
  nodeHeardCount[nodeId] = nodeHeardCount[nodeId] + 1;
}


/* ============================================================================
 *  SECTION 7: CSI + ESP-NOW SETUP
 *  Same settings as Espressif's csi_recv example.
 * ========================================================================== */
void check(esp_err_t err, const char *what) {
  if (err == ESP_OK) return;
  Serial.printf("[ERROR] %s failed: %s\n", what, esp_err_to_name(err));
  Serial.println("[ERROR] Stopped. Fix the problem above and press RESET.");
  while (true) delay(1000);
}

void csiInit() {
  // Promiscuous mode: the radio passes up every frame on the channel,
  // including ESP-NOW broadcasts from boards that never joined the WiFi.
  // The S3 stays connected to the access point at the same time.
  check(esp_wifi_set_promiscuous(true), "Enable promiscuous mode");

  // ESP-NOW is started so its frames are processed (and counted, SECTION 6)
  check(esp_now_init(), "Start ESP-NOW");
  check(esp_now_register_recv_cb(onEspNowReceived), "Register ESP-NOW callback");

  // --- Which parts of each packet to measure CSI on ---
#if CONFIG_IDF_TARGET_ESP32C6
  // (Used only if you build this receiver for an ESP32-C6 instead)
  wifi_csi_config_t csiConfig = {};
  csiConfig.enable                 = true;
  csiConfig.acquire_csi_legacy     = true;   // 802.11g packets
  csiConfig.acquire_csi_ht20       = true;   // 802.11n 20 MHz packets (ours)
  csiConfig.acquire_csi_ht40       = true;
  csiConfig.acquire_csi_su         = false;
  csiConfig.acquire_csi_mu         = false;
  csiConfig.acquire_csi_dcm        = false;
  csiConfig.acquire_csi_beamformed = false;
  csiConfig.acquire_csi_he_stbc    = 2;
  csiConfig.val_scale_cfg          = 0;
  csiConfig.dump_ack_en            = false;
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32C3
  // ESP32-S3 (and ESP32 / C3) settings, as in Espressif's csi_recv:
  wifi_csi_config_t csiConfig = {};
  csiConfig.lltf_en           = true;   // Legacy training field (every packet has it)
  csiConfig.htltf_en          = true;   // 802.11n training field (our packets have it)
  csiConfig.stbc_htltf2_en    = true;   // Second field, only on STBC packets
  csiConfig.ltf_merge_en      = true;   // Combine the fields where possible
  csiConfig.channel_filter_en = true;   // Smooth neighbouring subcarriers
  csiConfig.manu_scale        = false;  // Let the hardware scale the values
  csiConfig.shift             = 0;
#else
  #error "This receiver supports ESP32-S3, ESP32, ESP32-C3 and ESP32-C6."
#endif

  check(esp_wifi_set_csi_config(&csiConfig), "Set CSI config");
  check(esp_wifi_set_csi_rx_cb(onCsiReceived, nullptr), "Register CSI callback");
  check(esp_wifi_set_csi(true), "Enable CSI");

  Serial.println("[CSI] Capturing CSI from transmitters 1a:00:00:00:00:xx");
}


/* ============================================================================
 *  SECTION 8: UDP BATCHING AND SENDING
 *  Samples are packed into one UDP packet (layout in csi_packet.h) until
 *  it's full or BATCH_FLUSH_MS old, then sent to the Pi.
 * ========================================================================== */
static uint8_t  batchBuffer[MAX_UDP_PAYLOAD];
static size_t   batchUsed     = sizeof(csi_batch_header_t);
static uint8_t  batchSamples  = 0;
static uint32_t batchSeq      = 0;
static uint32_t batchStartMs  = 0;

void flushBatch() {
  if (batchSamples == 0) return;

  csi_batch_header_t header = {};
  header.magic        = CSI_PACKET_MAGIC;
  header.version      = CSI_PACKET_VERSION;
  header.receiver_id  = RECEIVER_ID;
  header.batch_seq    = ++batchSeq;
  header.sample_count = batchSamples;
  memcpy(batchBuffer, &header, sizeof(header));

  bool ok = false;
  if (WiFi.status() == WL_CONNECTED && udp.beginPacket(piAddress, PI_SERVER_PORT)) {
    udp.write(batchBuffer, batchUsed);
    ok = udp.endPacket();
  }
  if (ok) {
    udpPacketsSent++;
    samplesSent += batchSamples;
  } else {
    udpErrors++;
  }

  batchUsed    = sizeof(csi_batch_header_t);
  batchSamples = 0;
}

void addToBatch(const CsiQueueItem &item) {
  size_t needed = sizeof(csi_sample_header_t) + item.header.csi_len;

  // Won't fit -> send what we have first
  if (batchSamples > 0 && batchUsed + needed > MAX_UDP_PAYLOAD) {
    flushBatch();
  }
  // A single sample bigger than a whole packet can't be sent at all
  if (sizeof(csi_batch_header_t) + needed > MAX_UDP_PAYLOAD) {
    udpErrors++;
    return;
  }

  if (batchSamples == 0) batchStartMs = millis();
  memcpy(batchBuffer + batchUsed, &item.header, sizeof(item.header));
  batchUsed += sizeof(item.header);
  memcpy(batchBuffer + batchUsed, item.data, item.header.csi_len);
  batchUsed += item.header.csi_len;
  batchSamples++;

  if (batchSamples == 255) flushBatch();      // sample_count is one byte
}


/* ============================================================================
 *  SECTION 9: STATS PRINTER
 *  Prints one line per transmitter. Healthy: every node near its
 *  SEND_RATE_HZ, "queue full" and "UDP errors" at 0.
 * ========================================================================== */
void printStats() {
  static uint32_t lastCsi[256];
  static uint32_t lastHeard[256];
  const float seconds = STATS_PRINT_INTERVAL_MS / 1000.0f;

  Serial.printf("\n[STATS] WiFi %s | CSI total %lu | sent to Pi %lu samples in %lu UDP packets"
                " | queue full %lu | UDP errors %lu\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
                (unsigned long)totalCsi, (unsigned long)samplesSent,
                (unsigned long)udpPacketsSent, (unsigned long)queueFullDrops,
                (unsigned long)udpErrors);

  int activeNodes = 0;
  for (int node = 1; node < 255; node++) {
    uint32_t csi   = nodeCsiCount[node];
    uint32_t heard = nodeHeardCount[node];
    if (csi == 0 && heard == 0) continue;    // Never seen this node
    activeNodes++;
    Serial.printf("  node %3d | CSI/s %5.1f | heard/s %5.1f | RSSI %4d dBm | CSI %u bytes\n",
                  node, (csi - lastCsi[node]) / seconds, (heard - lastHeard[node]) / seconds,
                  nodeLastRssi[node], nodeCsiLen[node]);
    lastCsi[node]   = csi;
    lastHeard[node] = heard;
  }
  if (activeNodes == 0) {
    Serial.println("  No transmitters heard yet. Check they're powered and on the");
    Serial.printf ("  same channel as the access point (channel %d).\n", (int)WiFi.channel());
  }
}


/* ============================================================================
 *  SECTION 10: setup()  -  runs once at power-on / reset
 * ========================================================================== */
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1500);   // Give the USB serial port a moment to come up

  Serial.println("==================================================");
  Serial.printf ("  CSI receiver %d  |  sending to %s:%d\n",
                 RECEIVER_ID, PI_SERVER_IP, PI_SERVER_PORT);
  Serial.println("==================================================");

  ledInit();
  ledSet(false);

  if (!piAddress.fromString(PI_SERVER_IP)) {
    Serial.println("[ERROR] PI_SERVER_IP in config.h is not a valid IP address.");
    while (true) delay(1000);
  }

  csiQueue = xQueueCreate(CSI_QUEUE_LENGTH, sizeof(CsiQueueItem));
  if (csiQueue == nullptr) {
    Serial.println("[ERROR] Not enough memory for the CSI queue. Lower CSI_QUEUE_LENGTH.");
    while (true) delay(1000);
  }

  connectWiFi();
  ledSet(true);
  udp.begin(0);     // 0 = any free local port for sending
  csiInit();
}


/* ============================================================================
 *  SECTION 11: loop()  -  runs over and over forever
 * ========================================================================== */
void loop() {
  static CsiQueueItem item;              // "static": too big for the stack
  static uint32_t nextStatsMs = millis() + STATS_PRINT_INTERVAL_MS;
  static uint32_t lastBlinkMs = 0;
  static uint32_t lastTotal   = 0;
  static bool     ledOn       = true;

  // --- 1. Move every waiting CSI sample into the batch ---
  //     (waits up to 10 ms for the first one, so loop() isn't spinning)
  if (xQueueReceive(csiQueue, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
    do {
      addToBatch(item);
    } while (xQueueReceive(csiQueue, &item, 0) == pdTRUE);
  }

  // --- 2. Don't let samples wait too long when data is slow ---
  if (batchSamples > 0 && millis() - batchStartMs >= BATCH_FLUSH_MS) {
    flushBatch();
  }

  // --- 3. Status LED ---
  if (WiFi.status() != WL_CONNECTED) {
    ledOn = false;                                   // off: no WiFi
    ledSet(false);
  } else if (millis() - lastBlinkMs >= 500) {
    lastBlinkMs = millis();
    bool csiFlowing = (totalCsi != lastTotal);
    lastTotal = totalCsi;
    ledOn = csiFlowing ? !ledOn : true;              // blink if CSI, else solid
    ledSet(ledOn);
  }

  // --- 4. Stats ---
  if ((int32_t)(millis() - nextStatsMs) >= 0) {
    printStats();
    nextStatsMs += STATS_PRINT_INTERVAL_MS;
  }
}

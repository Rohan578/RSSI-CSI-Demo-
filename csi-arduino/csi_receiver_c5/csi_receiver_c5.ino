/*
 * ============================================================================
 *  DUAL-BAND CSI RECEIVER  -  ESP32-C5, Arduino IDE version
 * ============================================================================
 *
 *  WHAT THIS BOARD DOES
 *    1. Joins the WiFi access point on 2.4 GHz OR 5 GHz (USE_5GHZ in config.h).
 *    2. Collects CSI from up to two kinds of source:
 *         a) THE ACCESS POINT: the C5 pings it PING_RATE_HZ times per second
 *            and measures CSI on every reply (link: access point -> C5).
 *            Works on both bands. On 5 GHz this is the main source.
 *         b) TRANSMITTER BOARDS: ESP-NOW packets from the transmitter sketch
 *            (same as the S3 does). On 5 GHz only C5 transmitters work.
 *    3. Packs the CSI into UDP packets and sends them to the Pi, in the
 *       same format as the S3 - so the Pi logs both receivers side by side.
 *
 *  WHY THE ACCESS POINT TRICK
 *    The C5 is one radio: it hears ONE channel at a time. Your ESP32 / C6 /
 *    C3 transmitters can only send on 2.4 GHz, so on 5 GHz something else
 *    has to send packets. The access point already does: every ping gets a
 *    reply from it. Espressif's "csi_recv_router" example uses this method.
 *
 *  DATA FLOW INSIDE THIS SKETCH
 *
 *     WiFi radio ──> onCsiReceived() ──> queue ──> loop() ──> UDP ──> Pi
 *                    (inside the WiFi               (batches samples,
 *                     driver: FAST, only             sends them, prints
 *                     copies)                        stats)
 *
 *  BASED ON
 *    Espressif's "csi_recv_router" and "csi_recv" examples
 *    (github.com/espressif/esp-csi, examples/get-started/).
 *
 *  ARDUINO IDE SETUP
 *    - Boards Manager: "esp32 by Espressif Systems", version 3.3.0 or newer
 *    - Tools > Board:  "ESP32C5 Dev Module"  (or your exact C5 board)
 *    - Tools > USB CDC On Boot: "Enabled" if plugged into the C5's native
 *      USB port, "Disabled" if plugged into a USB-to-serial (UART) port
 *    - Serial Monitor: 115200 baud
 *
 *  FILES (tabs at the top of the Arduino IDE)
 *    config.h          <- band, WiFi, Pi IP address, sources. EDIT THIS.
 *    csi_packet.h      <- UDP format sent to the Pi (server.py must match)
 *    espnow_packet.h   <- what transmitters send (must match transmitter)
 *    csi_receiver_c5   <- this file
 *
 *  SECTIONS IN THIS FILE
 *    1. Includes and shared state
 *    2. Status LED
 *    3. WiFi connection (band selection)
 *    4. Reading a transmitter's packet counter
 *    5. CSI callback (the fast part)
 *    6. ESP-NOW callback (counts transmitter packets heard)
 *    7. CSI + ESP-NOW setup
 *    8. Pinging the access point
 *    9. UDP batching and sending
 *   10. Stats printer
 *   11. setup()  - runs once
 *   12. loop()   - runs forever
 * ============================================================================
 */


/* ============================================================================
 *  SECTION 1: INCLUDES AND SHARED STATE
 * ========================================================================== */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "ping/ping_sock.h"

#include "config.h"
#include "csi_packet.h"
#include "espnow_packet.h"

#if !CONFIG_SOC_WIFI_SUPPORT_5G
  #error "This sketch is for the ESP32-C5 (dual-band). For the S3, use csi_receiver_s3."
#endif

#define AP_NODE_ID 0   // "node 0" on the Pi = the access point

// One CSI sample as it travels from the WiFi callback to loop()
typedef struct {
  csi_sample_header_t header;
  int8_t              data[CSI_MAX_LEN];
} CsiQueueItem;

static QueueHandle_t csiQueue;                  // Callback -> loop()
static const uint8_t nodePrefix[5] = NODE_MAC_PREFIX;

WiFiUDP   udp;
IPAddress piAddress;

// Who counts as "the access point" (filled in after connecting)
static uint8_t       apBssid[6];                // Access point's radio MAC
static uint8_t       myMac[6];                  // This C5's MAC
static volatile bool apKnown = false;

// Per-source counters (index = node ID, index 0 = access point)
static volatile uint32_t nodeCsiCount[256];     // CSI samples captured
static volatile uint32_t nodeHeardCount[256];   // ESP-NOW packets heard
static volatile int8_t   nodeLastRssi[256];     // Most recent signal strength
static volatile uint16_t nodeCsiLen[256];       // CSI size from this source

// Overall counters
static volatile uint32_t totalCsi       = 0;
static volatile uint32_t queueFullDrops = 0;
static volatile uint32_t pingReplies    = 0;
static volatile uint32_t pingTimeouts   = 0;
static uint32_t udpPacketsSent = 0;
static uint32_t samplesSent    = 0;
static uint32_t udpErrors      = 0;


/* ============================================================================
 *  SECTION 2: STATUS LED
 *    off       = not connected to WiFi
 *    solid on  = connected, but no CSI arriving yet
 *    blinking  = connected and CSI is flowing
 * ========================================================================== */
#if STATUS_LED_GPIO == -2
  #ifdef LED_BUILTIN
    static const int ledPin = LED_BUILTIN;   // Board's own LED
  #else
    static const int ledPin = -1;
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
 *  SECTION 3: WIFI CONNECTION (band selection)
 *  "check()" stops with a clear message if a step fails.
 * ========================================================================== */
void check(esp_err_t err, const char *what) {
  if (err == ESP_OK) return;
  Serial.printf("[ERROR] %s failed: %s\n", what, esp_err_to_name(err));
  Serial.println("[ERROR] Stopped. Fix the problem above and press RESET.");
  while (true) delay(1000);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);           // Starts the radio (Arduino sets band to "auto" here)

  // --- Lock to ONE band. Must come after WiFi.mode(). ---
  if (!WiFi.setBandMode(USE_5GHZ ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY)) {
    check(ESP_FAIL, "Set WiFi band");
  }

  // --- 802.11n at 20 MHz on both bands ---
  // Turning off WiFi 6 (11ax) and 11ac makes the access point answer with
  // 802.11n packets - the same kind of packet on both bands, so the CSI from
  // 2.4 GHz and 5 GHz can be compared directly. 20 MHz keeps the number of
  // subcarriers the same on both bands too.
  wifi_protocols_t protocols = {};
  protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
  check(esp_wifi_set_protocols(WIFI_IF_STA, &protocols), "Set WiFi protocol");

  wifi_bandwidths_t widths = {};
  widths.ghz_2g = WIFI_BW_HT20;
  widths.ghz_5g = WIFI_BW_HT20;
  check(esp_wifi_set_bandwidths(WIFI_IF_STA, &widths), "Set 20 MHz bandwidth");

  WiFi.setSleep(false);          // No power saving: never miss a packet
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] Connecting to \"%s\" on %s", WIFI_SSID, USE_5GHZ ? "5 GHz" : "2.4 GHz");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[WIFI] Still not connected. Check WIFI_SSID / WIFI_PASSWORD.");
      if (USE_5GHZ) {
        Serial.println("[WIFI] Does the access point have a 5 GHz network with this name?");
        Serial.println("[WIFI] Some use a separate name for 5 GHz, like \"MyWiFi-5G\".");
      }
      Serial.println("[WIFI] Retrying...");
      start = millis();
    }
  }

  // Remember the access point's radio MAC and our own MAC, so the CSI
  // callback can pick out the access point's replies to us.
  memcpy(apBssid, WiFi.BSSID(), 6);
  WiFi.macAddress(myMac);
  apKnown = true;

  int channel = (int)WiFi.channel();
  Serial.println();
  Serial.printf("[WIFI] Connected. C5 IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Signal from access point: %d dBm\n", (int)WiFi.RSSI());
  Serial.println("--------------------------------------------------");
  Serial.printf ("  BAND: %s    ACCESS POINT CHANNEL: %d\n", channel >= 36 ? "5 GHz" : "2.4 GHz", channel);
  if (CSI_FROM_TRANSMITTERS) {
    Serial.println("  Transmitters on this band must use this channel.");
  }
  if (channel >= 52 && channel <= 144) {
    Serial.println("  WARNING: this is a DFS (radar) channel. The access point may");
    Serial.println("  switch channels without warning. Use 36-48 or 149-165.");
  }
  Serial.println("--------------------------------------------------");
}


/* ============================================================================
 *  SECTION 4: READING A TRANSMITTER'S PACKET COUNTER
 *  Searches the start of the received frame for our tx_packet_t (magic +
 *  version + node ID) and returns its counter. 0 = not found.
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
 *  Called by the WiFi driver for EVERY packet received from any device.
 *  Keeps only:
 *    - transmitter boards (MAC 1a:00:00:00:00:xx)            -> node 1-254
 *    - the access point's packets addressed to this C5 (ping replies) -> node 0
 *  Never print or wait here.
 * ========================================================================== */
void onCsiReceived(void *ctx, wifi_csi_info_t *info) {
  if (info == nullptr || info->buf == nullptr) return;

  uint8_t  nodeId;
  uint32_t txSeq;

  if (CSI_FROM_TRANSMITTERS && memcmp(info->mac, nodePrefix, sizeof(nodePrefix)) == 0 &&
      info->mac[5] != 0 && info->mac[5] != 255) {
    // --- A transmitter board ---
    nodeId = info->mac[5];
    txSeq  = findTxSeq(info->payload, info->payload_len, nodeId);

  } else if (CSI_FROM_ACCESS_POINT && apKnown &&
             memcmp(info->mac, apBssid, 6) == 0 && memcmp(info->dmac, myMac, 6) == 0) {
    // --- The access point, in a packet sent to us (a ping reply) ---
    // Beacons (sent to everyone) are skipped: they use a different, older
    // packet type, so mixing them in would make the CSI inconsistent.
    nodeId = AP_NODE_ID;
    txSeq  = 0;                                   // No counter for the AP

  } else {
    return;                                       // Someone else - ignore
  }

  static CsiQueueItem item;                       // Off the WiFi task's stack
  const wifi_pkt_rx_ctrl_t *rx = &info->rx_ctrl;

  item.header.tx_node_id         = nodeId;
  item.header.rssi               = (int8_t)rx->rssi;
  item.header.noise_floor        = (int8_t)rx->noise_floor;
  item.header.channel            = (uint8_t)rx->channel;
  item.header.rx_format          = (uint8_t)rx->cur_bb_format;
  item.header.first_word_invalid = info->first_word_invalid ? 1 : 0;
  item.header.tx_seq             = txSeq;
  item.header.rx_timestamp_us    = rx->timestamp;

  uint16_t len = info->len;
  if (len > CSI_MAX_LEN) len = CSI_MAX_LEN;
  item.header.csi_len = len;
  memcpy(item.data, info->buf, len);

  nodeCsiCount[nodeId] = nodeCsiCount[nodeId] + 1;
  nodeLastRssi[nodeId] = item.header.rssi;
  nodeCsiLen[nodeId]   = len;
  totalCsi = totalCsi + 1;

  if (xQueueSend(csiQueue, &item, 0) != pdTRUE) {
    queueFullDrops = queueFullDrops + 1;
  }
}


/* ============================================================================
 *  SECTION 6: ESP-NOW CALLBACK  (counts transmitter packets heard)
 *  Debug aid: heard but no CSI = CSI setting; not heard = channel/band/power.
 * ========================================================================== */
void onEspNowReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (info == nullptr || info->src_addr == nullptr) return;
  if (memcmp(info->src_addr, nodePrefix, sizeof(nodePrefix)) != 0) return;
  uint8_t nodeId = info->src_addr[5];
  nodeHeardCount[nodeId] = nodeHeardCount[nodeId] + 1;
}


/* ============================================================================
 *  SECTION 7: CSI + ESP-NOW SETUP
 * ========================================================================== */
void csiInit() {
  if (CSI_FROM_TRANSMITTERS) {
    // Promiscuous mode passes up ESP-NOW frames from boards that never
    // joined the WiFi. The C5 stays connected to the access point.
    check(esp_wifi_set_promiscuous(true), "Enable promiscuous mode");
    check(esp_now_init(), "Start ESP-NOW");
    check(esp_now_register_recv_cb(onEspNowReceived), "Register ESP-NOW callback");
  }

  // --- Which packets to measure CSI on (ESP32-C5 settings) ---
  wifi_csi_config_t csiConfig = {};
  csiConfig.enable                   = true;
  csiConfig.acquire_csi_legacy       = true;   // 802.11a/g packets
  csiConfig.acquire_csi_force_lltf   = false;
  csiConfig.acquire_csi_ht20         = true;   // 802.11n 20 MHz (ping replies + transmitters)
  csiConfig.acquire_csi_ht40         = true;   // 802.11n 40 MHz
  csiConfig.acquire_csi_vht          = false;  // 802.11ac (turned off in SECTION 3)
  csiConfig.acquire_csi_su           = false;  // WiFi 6 (turned off in SECTION 3)
  csiConfig.acquire_csi_mu           = false;
  csiConfig.acquire_csi_dcm          = false;
  csiConfig.acquire_csi_beamformed   = false;
  csiConfig.acquire_csi_he_stbc_mode = 2;
  csiConfig.val_scale_cfg            = 0;
  csiConfig.dump_ack_en              = false;
  csiConfig.lltf_bit_mode            = 1;      // 8-bit values, same as the S3 and C6

  check(esp_wifi_set_csi_config(&csiConfig), "Set CSI config");
  check(esp_wifi_set_csi_rx_cb(onCsiReceived, nullptr), "Register CSI callback");
  check(esp_wifi_set_csi(true), "Enable CSI");

  Serial.printf("[CSI] Sources: %s%s\n",
                CSI_FROM_ACCESS_POINT ? "access point (node 0)  " : "",
                CSI_FROM_TRANSMITTERS ? "transmitters 1a:00:00:00:00:xx" : "");
}


/* ============================================================================
 *  SECTION 8: PINGING THE ACCESS POINT
 *  The pings exist only to make the access point send packets back to us.
 *  Every reply = one CSI sample on the access point -> C5 link.
 * ========================================================================== */
void onPingReply(esp_ping_handle_t handle, void *args)   { pingReplies  = pingReplies + 1; }
void onPingTimeout(esp_ping_handle_t handle, void *args) { pingTimeouts = pingTimeouts + 1; }

void pingStart() {
  IPAddress target = WiFi.gatewayIP();
  if (strlen(PING_TARGET_IP) > 0 && !target.fromString(PING_TARGET_IP)) {
    Serial.println("[ERROR] PING_TARGET_IP in config.h is not a valid IP address.");
    while (true) delay(1000);
  }

  esp_ping_config_t pingCfg = ESP_PING_DEFAULT_CONFIG();
  pingCfg.count           = ESP_PING_COUNT_INFINITE;   // Ping forever
  pingCfg.interval_ms     = 1000 / PING_RATE_HZ;
  pingCfg.timeout_ms      = 1000;
  pingCfg.data_size       = 1;                         // Smallest possible ping
  pingCfg.task_stack_size = 3072;
  pingCfg.target_addr.type = IPADDR_TYPE_V4;
  pingCfg.target_addr.u_addr.ip4.addr = (uint32_t)target;

  esp_ping_callbacks_t callbacks = {};
  callbacks.on_ping_success = onPingReply;
  callbacks.on_ping_timeout = onPingTimeout;

  esp_ping_handle_t pingHandle = nullptr;
  check(esp_ping_new_session(&pingCfg, &callbacks, &pingHandle), "Create ping session");
  check(esp_ping_start(pingHandle), "Start pinging");

  Serial.printf("[PING] Pinging %s %d times per second\n", target.toString().c_str(), PING_RATE_HZ);
}


/* ============================================================================
 *  SECTION 9: UDP BATCHING AND SENDING  (same as the S3 receiver)
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

  if (batchSamples > 0 && batchUsed + needed > MAX_UDP_PAYLOAD) {
    flushBatch();                                 // Won't fit: send first
  }
  if (sizeof(csi_batch_header_t) + needed > MAX_UDP_PAYLOAD) {
    udpErrors++;                                  // Can never fit
    return;
  }

  if (batchSamples == 0) batchStartMs = millis();
  memcpy(batchBuffer + batchUsed, &item.header, sizeof(item.header));
  batchUsed += sizeof(item.header);
  memcpy(batchBuffer + batchUsed, item.data, item.header.csi_len);
  batchUsed += item.header.csi_len;
  batchSamples++;

  if (batchSamples == 255) flushBatch();          // sample_count is one byte
}


/* ============================================================================
 *  SECTION 10: STATS PRINTER
 *  Healthy: access point CSI/s near PING_RATE_HZ, transmitters near their
 *  SEND_RATE_HZ, "queue full" and "UDP errors" at 0.
 * ========================================================================== */
void printStats() {
  static uint32_t lastCsi[256];
  static uint32_t lastHeard[256];
  static uint32_t lastReplies = 0;
  const float seconds = STATS_PRINT_INTERVAL_MS / 1000.0f;
  int channel = (int)WiFi.channel();

  Serial.printf("\n[STATS] %s ch %d | WiFi %s | CSI total %lu | sent %lu samples in %lu UDP packets"
                " | queue full %lu | UDP errors %lu\n",
                channel >= 36 ? "5 GHz" : "2.4 GHz", channel,
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
                (unsigned long)totalCsi, (unsigned long)samplesSent,
                (unsigned long)udpPacketsSent, (unsigned long)queueFullDrops,
                (unsigned long)udpErrors);

  if (CSI_FROM_ACCESS_POINT) {
    uint32_t csi = nodeCsiCount[AP_NODE_ID];
    uint32_t replies = pingReplies;
    Serial.printf("  access pt | CSI/s %5.1f | pings ok/s %5.1f | RSSI %4d dBm | CSI %u bytes | ping timeouts %lu\n",
                  (csi - lastCsi[AP_NODE_ID]) / seconds, (replies - lastReplies) / seconds,
                  nodeLastRssi[AP_NODE_ID], nodeCsiLen[AP_NODE_ID], (unsigned long)pingTimeouts);
    lastCsi[AP_NODE_ID] = csi;
    lastReplies = replies;
  }

  if (CSI_FROM_TRANSMITTERS) {
    int active = 0;
    for (int node = 1; node < 255; node++) {
      uint32_t csi   = nodeCsiCount[node];
      uint32_t heard = nodeHeardCount[node];
      if (csi == 0 && heard == 0) continue;
      active++;
      Serial.printf("  node %4d | CSI/s %5.1f | heard/s   %5.1f | RSSI %4d dBm | CSI %u bytes\n",
                    node, (csi - lastCsi[node]) / seconds, (heard - lastHeard[node]) / seconds,
                    nodeLastRssi[node], nodeCsiLen[node]);
      lastCsi[node]   = csi;
      lastHeard[node] = heard;
    }
    if (active == 0) {
      Serial.printf("  No transmitters heard on channel %d (normal on 5 GHz unless you\n", channel);
      Serial.println("  have a C5 transmitter on this channel).");
    }
  }
}


/* ============================================================================
 *  SECTION 11: setup()  -  runs once at power-on / reset
 * ========================================================================== */
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1500);

  Serial.println("==================================================");
  Serial.printf ("  Dual-band CSI receiver %d (ESP32-C5)  |  %s\n",
                 RECEIVER_ID, USE_5GHZ ? "5 GHz" : "2.4 GHz");
  Serial.printf ("  Sending to %s:%d\n", PI_SERVER_IP, PI_SERVER_PORT);
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
  udp.begin(0);
  csiInit();
  if (CSI_FROM_ACCESS_POINT) pingStart();
}


/* ============================================================================
 *  SECTION 12: loop()  -  runs over and over forever
 * ========================================================================== */
void loop() {
  static CsiQueueItem item;
  static uint32_t nextStatsMs = millis() + STATS_PRINT_INTERVAL_MS;
  static uint32_t lastBlinkMs = 0;
  static uint32_t lastTotal   = 0;
  static bool     ledOn       = true;

  // --- 1. Move every waiting CSI sample into the batch ---
  if (xQueueReceive(csiQueue, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
    do {
      addToBatch(item);
    } while (xQueueReceive(csiQueue, &item, 0) == pdTRUE);
  }

  // --- 2. Don't let samples wait too long ---
  if (batchSamples > 0 && millis() - batchStartMs >= BATCH_FLUSH_MS) {
    flushBatch();
  }

  // --- 3. Status LED ---
  if (WiFi.status() != WL_CONNECTED) {
    ledOn = false;
    ledSet(false);
  } else if (millis() - lastBlinkMs >= 500) {
    lastBlinkMs = millis();
    bool csiFlowing = (totalCsi != lastTotal);
    lastTotal = totalCsi;
    ledOn = csiFlowing ? !ledOn : true;
    ledSet(ledOn);
  }

  // --- 4. Stats ---
  if ((int32_t)(millis() - nextStatsMs) >= 0) {
    printStats();
    nextStatsMs += STATS_PRINT_INTERVAL_MS;
  }
}

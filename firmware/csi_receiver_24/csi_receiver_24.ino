/*
 * ============================================================================
 *  2.4 GHz CSI RECEIVER  -  home WiFi version, Arduino IDE
 *  Boards: ESP32-S3 (Seeed XIAO ESP32S3 / DevKit) - the main receiver
 *          ESP32-C6 (SuperMini)                    - optional 2nd receiver
 * ============================================================================
 *
 *  WHAT THIS BOARD DOES
 *    1. Joins your home WiFi (2.4 GHz) - that's how it reaches the laptop.
 *    2. Broadcasts a small "I'm on channel N" beacon, 5 times a second, so
 *       the transmitters find its channel and follow it when the router
 *       changes channel.
 *    3. Listens (promiscuous mode) for the transmitters' ESP-NOW packets.
 *       For each one, the WiFi hardware measures CSI: how every subcarrier
 *       was weakened on its way across the room.
 *    4. Packs the CSI into UDP packets and sends them to the laptop running
 *       server/server.py, which it finds by itself (the server announces
 *       itself on the WiFi once a second).
 *
 *  DATA FLOW
 *     WiFi radio ──> onCsiReceived() ──> queue ──> loop() ──> UDP ──> laptop
 *                    (inside the WiFi               (batches, beacons,
 *                     driver: FAST, only copies)     discovery, stats)
 *
 *  WHAT'S NEW IN THIS VERSION (vs. flash-ready)
 *    - Sends to a laptop on the home WiFi instead of a Raspberry Pi, and
 *      finds it automatically (no IP address to type in).
 *    - Channel beacons for the transmitters (see above).
 *    - Sends only the 128-byte LLTF block the server uses (a third of the
 *      traffic), and a bigger queue for up to 10 transmitters.
 *    - Builds for the ESP32-C6 too, for a second 2.4 GHz receiver.
 *
 *  BASED ON
 *    Espressif's "csi_recv" example (github.com/espressif/esp-csi) and the
 *    team's flash-ready S3 receiver.
 *
 *  ARDUINO IDE SETUP
 *    - Boards Manager: "esp32 by Espressif Systems", 3.3.0 or newer
 *    - Tools > Board: "XIAO_ESP32S3" (Seeed XIAO) or "ESP32S3 Dev Module",
 *                     or "ESP32C6 Dev Module" for a C6 SuperMini
 *    - Tools > USB CDC On Boot: "Enabled" for the XIAO and the C6, and for a
 *      DevKit plugged into its native "USB" port ("Disabled" on its "UART" port)
 *    - Serial Monitor: 115200 baud
 *
 *  FILES (tabs)
 *    config.h          <- WiFi, IDs, options. EDIT THIS.
 *    csi_packet.h      <- UDP format to the laptop (server.py must match)
 *    espnow_packet.h   <- packets in the air (identical in all three sketches)
 *    csi_receiver_24   <- this file
 *
 *  SECTIONS
 *    1. Includes and shared state       7. CSI + ESP-NOW setup, beacons
 *    2. Status LED                      8. Finding the laptop server
 *    3. WiFi connection                 9. UDP batching and sending
 *    4. Reading a transmitter's counter 10. Stats printer
 *    5. CSI callback (the fast part)    11. setup()
 *    6. ESP-NOW callback (counts)       12. loop()
 * ============================================================================
 */


/* ============================================================================
 *  SECTION 1: INCLUDES AND SHARED STATE
 * ========================================================================== */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <ESPmDNS.h>

#include "config.h"
#include "csi_packet.h"
#include "espnow_packet.h"

#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32C3)
  #error "csi_receiver_24 supports ESP32-S3, ESP32-C6, ESP32 and ESP32-C3. For the ESP32-C5 use csi_receiver_5g."
#endif

// The C6 lays its CSI out differently, so it always sends all of it
#if CONFIG_IDF_TARGET_ESP32C6
  #define SEND_BYTES 0
#else
  #define SEND_BYTES CSI_SEND_BYTES
#endif

static const uint8_t BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct {                                // One sample, callback -> loop()
  csi_sample_header_t header;
  int8_t              data[CSI_MAX_LEN];
} CsiQueueItem;

static QueueHandle_t csiQueue;
static const uint8_t nodePrefix[5] = NODE_MAC_PREFIX;

WiFiUDP   udp;                                  // CSI out to the laptop
WiFiUDP   discovery;                            // Server announcements in
IPAddress serverAddress;
uint16_t  serverPort  = SERVER_PORT;
bool      serverKnown = false;
const char *serverFoundBy = "not found yet";

// Per-transmitter counters (index = node ID), updated by the WiFi driver
static volatile uint32_t nodeCsiCount[256];
static volatile uint32_t nodeHeardCount[256];
static volatile int8_t   nodeLastRssi[256];
static volatile uint16_t nodeCsiLen[256];

// Overall counters
static volatile uint32_t totalCsi       = 0;
static volatile uint32_t queueFullDrops = 0;
static uint32_t udpPacketsSent = 0;
static uint32_t samplesSent    = 0;
static uint32_t udpErrors      = 0;
static uint32_t noServerDrops  = 0;             // Samples dropped: laptop not found yet
static uint32_t beaconsSent    = 0;


/* ============================================================================
 *  SECTION 2: STATUS LED
 *    off = no WiFi | fast blink = no server yet | solid = no CSI yet
 *    slow blink = working
 * ========================================================================== */
#if STATUS_LED_GPIO == -2
  #ifdef LED_BUILTIN
    static const int ledPin = LED_BUILTIN;
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
 *  SECTION 3: WIFI CONNECTION
 *  The Arduino WiFi library reconnects by itself if the connection drops.
 * ========================================================================== */
void check(esp_err_t err, const char *what) {
  if (err == ESP_OK) return;
  Serial.printf("[ERROR] %s failed: %s\n", what, esp_err_to_name(err));
  Serial.println("[ERROR] Stopped. Fix the problem above and press RESET.");
  while (true) delay(1000);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // No power saving: never miss a packet
  WiFi.setAutoReconnect(true);

  if (strlen(STATIC_IP) > 0) {   // Fixed address (config.h section 2)
    IPAddress ip, gateway, subnet;
    if (!ip.fromString(STATIC_IP) || !gateway.fromString(STATIC_GATEWAY) ||
        !subnet.fromString(STATIC_SUBNET)) {
      Serial.println("[ERROR] STATIC_IP / STATIC_GATEWAY / STATIC_SUBNET in config.h is not a valid IP.");
      while (true) delay(1000);
    }
    // DNS = the gateway, so name lookups still work
    if (WiFi.config(ip, gateway, subnet, gateway)) Serial.printf("[WIFI] Using fixed IP %s\n", STATIC_IP);
    else Serial.println("[WIFI] Could not set the fixed IP - using DHCP");
  }

  uint8_t bssid[6];              // Optional: one specific access point (1b)
  bool pinned = strlen(WIFI_BSSID) > 0 &&
                sscanf(WIFI_BSSID, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                       &bssid[0], &bssid[1], &bssid[2], &bssid[3], &bssid[4], &bssid[5]) == 6;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 0, pinned ? bssid : nullptr);

  Serial.printf("[WIFI] Connecting to \"%s\"%s%s", WIFI_SSID,
                pinned ? " via access point " : "", pinned ? WIFI_BSSID : "");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    if (pinned && millis() - start > 20000) {
      Serial.printf("\n[WIFI] Access point %s not reachable - joining any \"%s\"", WIFI_BSSID, WIFI_SSID);
      pinned = false;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
    if (millis() - start > 20000) {
      Serial.printf("\n[WIFI] Still not connected to \"%s\". Check WIFI_SSID / WIFI_PASSWORD,\n", WIFI_SSID);
      Serial.println("[WIFI] and that the network has a 2.4 GHz band in range. Retrying...");
      start = millis();
    }
  }

  Serial.println();
  Serial.printf("[WIFI] Connected. Receiver IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Access point: %s (put this in WIFI_BSSID to always use it)\n",
                WiFi.BSSIDstr().c_str());
  Serial.printf("[WIFI] Signal from access point: %d dBm\n", (int)WiFi.RSSI());
  Serial.println("--------------------------------------------------");
  Serial.printf ("  2.4 GHz CHANNEL: %d\n", (int)WiFi.channel());
  Serial.println("  Transmitters find this channel by themselves (receiver beacons).");
  Serial.println("--------------------------------------------------");
}


/* ============================================================================
 *  SECTION 4: READING A TRANSMITTER'S PACKET COUNTER
 *  The CSI callback also gets the frame's bytes. Our 12-byte tx_packet_t is
 *  near the start, after ESP-NOW's own header: find it by its magic number,
 *  version and node ID, then read the counter. 0 = not found.
 * ========================================================================== */
uint32_t findTxSeq(const uint8_t *payload, uint16_t payloadLen, uint8_t nodeId) {
  if (payload == nullptr || payloadLen < sizeof(tx_packet_t)) return 0;
  const uint8_t magicLow  = TX_PACKET_MAGIC & 0xFF;
  const uint8_t magicHigh = TX_PACKET_MAGIC >> 8;
  uint16_t searchEnd = payloadLen - sizeof(tx_packet_t);
  if (searchEnd > 64) searchEnd = 64;
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
 *  Called by the WiFi driver for every packet from every nearby device.
 *  Keep only our transmitters, copy into the queue, return. No printing.
 * ========================================================================== */
void onCsiReceived(void *ctx, wifi_csi_info_t *info) {
  if (info == nullptr || info->buf == nullptr) return;
  if (memcmp(info->mac, nodePrefix, sizeof(nodePrefix)) != 0) return;
  uint8_t nodeId = info->mac[5];
  if (nodeId == 0 || nodeId == 255) return;

  static CsiQueueItem item;                     // Off the WiFi task's small stack
  const wifi_pkt_rx_ctrl_t *rx = &info->rx_ctrl;

  item.header.tx_node_id         = nodeId;
  item.header.rssi               = (int8_t)rx->rssi;
  item.header.noise_floor        = (int8_t)rx->noise_floor;
  item.header.channel            = (uint8_t)rx->channel;
#if CONFIG_IDF_TARGET_ESP32C6
  item.header.rx_format          = (uint8_t)rx->cur_bb_format;
#else
  item.header.rx_format          = (uint8_t)rx->sig_mode;
#endif
  item.header.first_word_invalid = info->first_word_invalid ? 1 : 0;
  item.header.tx_seq             = findTxSeq(info->payload, info->payload_len, nodeId);
  item.header.rx_timestamp_us    = rx->timestamp;

  uint16_t len = info->len;
  if (SEND_BYTES > 0 && len > SEND_BYTES) len = SEND_BYTES;   // LLTF block only
  if (len > CSI_MAX_LEN) len = CSI_MAX_LEN;
  item.header.csi_len = len;
  memcpy(item.data, info->buf, len);

  nodeCsiCount[nodeId] = nodeCsiCount[nodeId] + 1;
  nodeLastRssi[nodeId] = item.header.rssi;
  nodeCsiLen[nodeId]   = len;
  totalCsi = totalCsi + 1;

  if (xQueueSend(csiQueue, &item, 0) != pdTRUE) queueFullDrops = queueFullDrops + 1;
}


/* ============================================================================
 *  SECTION 6: ESP-NOW CALLBACK  (counts transmitter packets heard)
 *  Debug aid: heard but no CSI = CSI setting; not heard = channel/power.
 * ========================================================================== */
void onEspNowReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (info == nullptr || info->src_addr == nullptr) return;
  if (memcmp(info->src_addr, nodePrefix, sizeof(nodePrefix)) != 0) return;
  uint8_t nodeId = info->src_addr[5];
  nodeHeardCount[nodeId] = nodeHeardCount[nodeId] + 1;
}


/* ============================================================================
 *  SECTION 7: CSI + ESP-NOW SETUP, BEACONS
 * ========================================================================== */
void csiInit() {
  // Promiscuous: the radio passes up ESP-NOW frames from boards that never
  // joined the WiFi, while staying connected to the router.
  check(esp_wifi_set_promiscuous(true), "Enable promiscuous mode");
  check(esp_now_init(), "Start ESP-NOW");
  check(esp_now_register_recv_cb(onEspNowReceived), "Register ESP-NOW callback");

  // Broadcast peer for the beacons. channel 0 = the channel we're on.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, sizeof(BROADCAST_ADDR));
  peer.channel = 0;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  check(esp_now_add_peer(&peer), "Add broadcast peer");

#if CONFIG_IDF_TARGET_ESP32C6
  wifi_csi_config_t csiConfig = {};
  csiConfig.enable                 = true;
  csiConfig.acquire_csi_legacy     = true;   // 802.11g packets
  csiConfig.acquire_csi_ht20       = true;   // 802.11n 20 MHz (our transmitters)
  csiConfig.acquire_csi_ht40       = true;
  csiConfig.acquire_csi_su         = false;
  csiConfig.acquire_csi_mu         = false;
  csiConfig.acquire_csi_dcm        = false;
  csiConfig.acquire_csi_beamformed = false;
  csiConfig.acquire_csi_he_stbc    = 2;
  csiConfig.val_scale_cfg          = 0;
  csiConfig.dump_ack_en            = false;
#else
  // ESP32-S3 / ESP32 / C3, as in Espressif's csi_recv. The buffer always
  // starts with the 128-byte LLTF block, which is what the server uses.
  wifi_csi_config_t csiConfig = {};
  csiConfig.lltf_en           = true;
  csiConfig.htltf_en          = true;
  csiConfig.stbc_htltf2_en    = true;
  csiConfig.ltf_merge_en      = true;
  csiConfig.channel_filter_en = true;
  csiConfig.manu_scale        = false;
  csiConfig.shift             = 0;
#endif
  check(esp_wifi_set_csi_config(&csiConfig), "Set CSI config");
  check(esp_wifi_set_csi_rx_cb(onCsiReceived, nullptr), "Register CSI callback");
  check(esp_wifi_set_csi(true), "Enable CSI");

  Serial.printf("[CSI] Capturing CSI from transmitters 1a:00:00:00:00:xx (%s)\n",
                SEND_BYTES > 0 ? "sending the 128-byte LLTF block" : "sending all CSI bytes");
}

void sendBeacon() {
  if (BEACON_RATE_HZ <= 0 || WiFi.status() != WL_CONNECTED) return;
  rx_beacon_t beacon = {};
  beacon.magic       = RX_BEACON_MAGIC;
  beacon.version     = RX_BEACON_VERSION;
  beacon.receiver_id = RECEIVER_ID;
  beacon.channel     = (uint8_t)WiFi.channel();
  if (esp_now_send(BROADCAST_ADDR, (const uint8_t *)&beacon, sizeof(beacon)) == ESP_OK) beaconsSent++;
}


/* ============================================================================
 *  SECTION 8: FINDING THE LAPTOP SERVER
 *  server.py broadcasts "CSI-SERVER v1 port=5005" on DISCOVERY_PORT once a
 *  second. Whoever sends it is the server: use its address, and switch if
 *  it changes (laptop got a new IP, or you moved to another computer).
 *  Before that: SERVER_HOST (mDNS) or SERVER_IP from config.h, if set.
 * ========================================================================== */
void checkDiscovery() {
  int size = discovery.parsePacket();
  if (size <= 0) return;
  char msg[64];
  int n = discovery.read((uint8_t *)msg, sizeof(msg) - 1);
  if (n <= 0) return;
  msg[n] = '\0';
  if (strncmp(msg, "CSI-SERVER", 10) != 0) return;

  uint16_t port = SERVER_PORT;
  const char *p = strstr(msg, "port=");
  if (p) {
    int v = atoi(p + 5);
    if (v > 0 && v < 65536) port = (uint16_t)v;
  }
  IPAddress from = discovery.remoteIP();
  if (!serverKnown || from != serverAddress || port != serverPort) {
    serverAddress = from;
    serverPort    = port;
    serverKnown   = true;
    serverFoundBy = "announcement";
    Serial.printf("[SERVER] Laptop server found at %s:%u\n", from.toString().c_str(), port);
  }
}

void findServerAtStart() {
  discovery.begin(DISCOVERY_PORT);
  if (strlen(SERVER_IP) > 0 && serverAddress.fromString(SERVER_IP)) {
    serverKnown = true;
    serverFoundBy = "SERVER_IP";
  }
  if (strlen(SERVER_HOST) > 0) {
    char name[24];
    snprintf(name, sizeof(name), "csi-receiver-%d", RECEIVER_ID);
    if (MDNS.begin(name)) {
      IPAddress ip = MDNS.queryHost(SERVER_HOST, 1500);
      if (ip != IPAddress()) {
        serverAddress = ip;
        serverKnown = true;
        serverFoundBy = "mDNS";
        Serial.printf("[SERVER] %s.local is at %s\n", SERVER_HOST, ip.toString().c_str());
      }
    }
  }
  // Give the announcement a few seconds (beacons keep going meanwhile)
  Serial.println("[SERVER] Waiting for the laptop server's announcement...");
  uint32_t start = millis();
  while (millis() - start < 4000) {
    checkDiscovery();
    if (strcmp(serverFoundBy, "announcement") == 0) return;
    static uint32_t lastBeacon = 0;
    if (BEACON_RATE_HZ > 0 && millis() - lastBeacon >= 1000 / BEACON_RATE_HZ) {
      lastBeacon = millis();
      sendBeacon();
    }
    delay(10);
  }
  if (serverKnown) {
    Serial.printf("[SERVER] Sending to %s (from %s); will switch if the server announces itself\n",
                  serverAddress.toString().c_str(), serverFoundBy);
  } else {
    Serial.println("[SERVER] Not found yet. Is server.py running on a laptop on this WiFi?");
    Serial.println("[SERVER] (Keeps listening. If it never appears, set SERVER_IP in config.h.)");
  }
}


/* ============================================================================
 *  SECTION 9: UDP BATCHING AND SENDING   (layout in csi_packet.h)
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

  if (!serverKnown) {
    noServerDrops += batchSamples;
  } else {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED && udp.beginPacket(serverAddress, serverPort)) {
      udp.write(batchBuffer, batchUsed);
      ok = udp.endPacket();
    }
    if (ok) { udpPacketsSent++; samplesSent += batchSamples; }
    else    { udpErrors++; }
  }
  batchUsed    = sizeof(csi_batch_header_t);
  batchSamples = 0;
}

void addToBatch(const CsiQueueItem &item) {
  size_t needed = sizeof(csi_sample_header_t) + item.header.csi_len;
  if (batchSamples > 0 && batchUsed + needed > MAX_UDP_PAYLOAD) flushBatch();
  if (sizeof(csi_batch_header_t) + needed > MAX_UDP_PAYLOAD) { udpErrors++; return; }
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
 *  Healthy: every node near its SEND_RATE_HZ; queue full / UDP errors 0.
 * ========================================================================== */
void printStats() {
  static uint32_t lastCsi[256];
  static uint32_t lastHeard[256];
  const float seconds = STATS_PRINT_INTERVAL_MS / 1000.0f;

  Serial.printf("\n[STATS] WiFi %s ch %d | server %s (%s) | CSI %lu | sent %lu in %lu UDP"
                " | queue full %lu | UDP errors %lu | no-server drops %lu | beacons %lu\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN", (int)WiFi.channel(),
                serverKnown ? serverAddress.toString().c_str() : "-", serverFoundBy,
                (unsigned long)totalCsi, (unsigned long)samplesSent, (unsigned long)udpPacketsSent,
                (unsigned long)queueFullDrops, (unsigned long)udpErrors,
                (unsigned long)noServerDrops, (unsigned long)beaconsSent);

  int active = 0;
  for (int node = 1; node < 255; node++) {
    uint32_t csi = nodeCsiCount[node], heard = nodeHeardCount[node];
    if (csi == 0 && heard == 0) continue;
    active++;
    Serial.printf("  node %3d | CSI/s %5.1f | heard/s %5.1f | RSSI %4d dBm | CSI %u bytes\n",
                  node, (csi - lastCsi[node]) / seconds, (heard - lastHeard[node]) / seconds,
                  nodeLastRssi[node], nodeCsiLen[node]);
    lastCsi[node] = csi;
    lastHeard[node] = heard;
  }
  if (active == 0) {
    Serial.println("  No transmitters heard yet. Are they powered? Their Serial Monitor");
    Serial.println("  should say LOCKED on this channel within ~10 s of power-up.");
  }
}


/* ============================================================================
 *  SECTION 11: setup()
 * ========================================================================== */
void setup() {
  Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);       // Never stall when no Serial Monitor is open
#endif
  delay(1500);

  Serial.println("==================================================");
  Serial.printf ("  2.4 GHz CSI receiver %d (home WiFi)\n", RECEIVER_ID);
  Serial.println("==================================================");

  ledInit();
  ledSet(false);

  if (strcmp(WIFI_PASSWORD, "__WIFI_PASSWORD__") == 0) {
    Serial.println("[ERROR] WIFI_PASSWORD in config.h is still the placeholder.");
    Serial.println("[ERROR] Run tools/fill_wifi_password.sh (or type it in) and upload again.");
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
  csiInit();                      // ESP-NOW first, so beacons can go out...
  findServerAtStart();            // ...while we look for the laptop
}


/* ============================================================================
 *  SECTION 12: loop()
 * ========================================================================== */
void loop() {
  static CsiQueueItem item;
  static uint32_t nextStatsMs = millis() + STATS_PRINT_INTERVAL_MS;
  static uint32_t lastBeaconMs = 0;
  static uint32_t lastBlinkMs = 0;
  static uint32_t lastTotal   = 0;
  static bool     ledOn       = true;

  // --- 1. Move waiting CSI samples into the batch (wait up to 5 ms) ---
  if (xQueueReceive(csiQueue, &item, pdMS_TO_TICKS(5)) == pdTRUE) {
    do { addToBatch(item); } while (xQueueReceive(csiQueue, &item, 0) == pdTRUE);
  }
  if (batchSamples > 0 && millis() - batchStartMs >= BATCH_FLUSH_MS) flushBatch();

  // --- 2. Beacons for the transmitters ---
  if (BEACON_RATE_HZ > 0 && millis() - lastBeaconMs >= 1000 / BEACON_RATE_HZ) {
    lastBeaconMs = millis();
    sendBeacon();
  }

  // --- 3. Server announcements (follow the laptop if its address changes) ---
  checkDiscovery();

  // --- 4. Status LED ---
  if (WiFi.status() != WL_CONNECTED) {
    ledOn = false;
    ledSet(false);
  } else if (!serverKnown) {
    ledSet((millis() / 100) % 2);                    // fast blink: no server
  } else if (millis() - lastBlinkMs >= 500) {
    lastBlinkMs = millis();
    bool csiFlowing = (totalCsi != lastTotal);
    lastTotal = totalCsi;
    ledOn = csiFlowing ? !ledOn : true;
    ledSet(ledOn);
  }

  // --- 5. Stats ---
  if ((int32_t)(millis() - nextStatsMs) >= 0) {
    printStats();
    nextStatsMs += STATS_PRINT_INTERVAL_MS;
  }
}

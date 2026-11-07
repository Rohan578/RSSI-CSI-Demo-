/*
 * ============================================================================
 *  5 GHz CSI RECEIVER  -  ESP32-C5 DevKit (4 MB), home WiFi version, Arduino
 * ============================================================================
 *
 *  WHAT THIS BOARD DOES
 *    1. Joins your home WiFi on 5 GHz (USE_5GHZ 1 in config.h).
 *    2. Pings the router PING_RATE_HZ times a second. The router answers
 *       every ping, and the C5's WiFi hardware measures CSI on each answer:
 *       that's the link "router -> C5" on 5 GHz ("node 0" on the laptop).
 *       The ESP32 / C3 / C6 transmitters can't send on 5 GHz - the router
 *       can, and it's already there.
 *    3. Sends the CSI to the laptop running server/server.py (found
 *       automatically, like the 2.4 GHz receiver), in the same format, so
 *       the server sees both bands side by side.
 *
 *    Why 5 GHz helps: half the wavelength (6 cm vs 12.5 cm), so it reacts to
 *    smaller movements; much less interference than 2.4 GHz (no microwaves,
 *    Bluetooth, or neighbours' 2.4 GHz WiFi); and it's an independent view,
 *    so the server can tell "2.4 GHz interference" from "a person moving".
 *
 *    With USE_5GHZ 0 the C5 is instead another 2.4 GHz receiver: it hears
 *    the transmitters and sends channel beacons, like csi_receiver_24.
 *
 *  BASED ON
 *    Espressif's "csi_recv_router" + "csi_recv" examples
 *    (github.com/espressif/esp-csi) and the team's flash-ready C5 receiver.
 *
 *  ARDUINO IDE SETUP
 *    - Boards Manager: "esp32 by Espressif Systems", 3.3.0 or newer
 *    - Tools > Board: "ESP32C5 Dev Module"
 *    - Tools > Flash Size: 4MB    Partition Scheme: Default 4MB
 *      (this sketch uses about 0.5 MB of the 1.25 MB app space)
 *    - Tools > USB CDC On Boot: "Enabled" on the C5's native USB port,
 *      "Disabled" on its USB-to-serial ("UART") port
 *    - Serial Monitor: 115200 baud
 *
 *  FILES (tabs)
 *    config.h          <- band, WiFi, options. EDIT THIS.
 *    csi_packet.h      <- UDP format to the laptop (server.py must match)
 *    espnow_packet.h   <- packets in the air (identical in all three sketches)
 *    csi_receiver_5g   <- this file
 *
 *  SECTIONS
 *    1. Includes and shared state       7. CSI + ESP-NOW setup, beacons
 *    2. Status LED                      8. Pinging the router
 *    3. WiFi connection (band)          9. Finding the laptop server
 *    4. Reading a transmitter's counter 10. UDP batching and sending
 *    5. CSI callback (the fast part)    11. Stats printer
 *    6. ESP-NOW callback (counts)       12. setup()   13. loop()
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
#include "ping/ping_sock.h"

#include "config.h"
#include "csi_packet.h"
#include "espnow_packet.h"

#if !CONFIG_SOC_WIFI_SUPPORT_5G
  #error "csi_receiver_5g is for the ESP32-C5 (dual-band). For the S3 or C6, use csi_receiver_24."
#endif

#define AP_NODE_ID 0                            // "node 0" on the laptop = the router
#define BEACONS_ON (!USE_5GHZ && CSI_FROM_TRANSMITTERS && BEACON_RATE_HZ > 0)

static const uint8_t BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct {
  csi_sample_header_t header;
  int8_t              data[CSI_MAX_LEN];
} CsiQueueItem;

static QueueHandle_t csiQueue;
static const uint8_t nodePrefix[5] = NODE_MAC_PREFIX;

WiFiUDP   udp;
WiFiUDP   discovery;
IPAddress serverAddress;
uint16_t  serverPort  = SERVER_PORT;
bool      serverKnown = false;
const char *serverFoundBy = "not found yet";

// The router's radio MAC and ours: the CSI callback keeps the router's
// packets addressed to us (the ping replies). Updated after a reconnect,
// because on a mesh the C5 may roam to another box.
static uint8_t       apBssid[6];
static uint8_t       myMac[6];
static volatile bool apKnown = false;

static volatile uint32_t nodeCsiCount[256];     // index 0 = the router
static volatile uint32_t nodeHeardCount[256];
static volatile int8_t   nodeLastRssi[256];
static volatile uint16_t nodeCsiLen[256];

static volatile uint32_t totalCsi       = 0;
static volatile uint32_t queueFullDrops = 0;
static volatile uint32_t pingReplies    = 0;
static volatile uint32_t pingTimeouts   = 0;
static uint32_t udpPacketsSent = 0;
static uint32_t samplesSent    = 0;
static uint32_t udpErrors      = 0;
static uint32_t noServerDrops  = 0;
static uint32_t beaconsSent    = 0;


/* ============================================================================
 *  SECTION 2: STATUS LED
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
 *  SECTION 3: WIFI CONNECTION (band selection)
 * ========================================================================== */
void check(esp_err_t err, const char *what) {
  if (err == ESP_OK) return;
  Serial.printf("[ERROR] %s failed: %s\n", what, esp_err_to_name(err));
  Serial.println("[ERROR] Stopped. Fix the problem above and press RESET.");
  while (true) delay(1000);
}

void rememberAccessPoint() {
  memcpy(apBssid, WiFi.BSSID(), 6);
  WiFi.macAddress(myMac);
  apKnown = true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);           // Starts the radio (Arduino sets the band to "auto")

  // Lock to ONE band. Must come after WiFi.mode().
  if (!WiFi.setBandMode(USE_5GHZ ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY)) {
    check(ESP_FAIL, "Set WiFi band");
  }

  // 802.11n at 20 MHz on both bands. With WiFi 6 / 11ac turned off, the
  // router answers with 802.11n packets: the same packet type and the same
  // 52 subcarriers as on 2.4 GHz, so the two bands are directly comparable.
  wifi_protocols_t protocols = {};
  protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
  check(esp_wifi_set_protocols(WIFI_IF_STA, &protocols), "Set WiFi protocol");
  wifi_bandwidths_t widths = {};
  widths.ghz_2g = WIFI_BW_HT20;
  widths.ghz_5g = WIFI_BW_HT20;
  check(esp_wifi_set_bandwidths(WIFI_IF_STA, &widths), "Set 20 MHz bandwidth");

  WiFi.setSleep(false);          // No power saving
  WiFi.setAutoReconnect(true);

  if (strlen(STATIC_IP) > 0) {
    IPAddress ip, gateway, subnet;
    if (!ip.fromString(STATIC_IP) || !gateway.fromString(STATIC_GATEWAY) ||
        !subnet.fromString(STATIC_SUBNET)) {
      Serial.println("[ERROR] STATIC_IP / STATIC_GATEWAY / STATIC_SUBNET in config.h is not a valid IP.");
      while (true) delay(1000);
    }
    if (WiFi.config(ip, gateway, subnet, gateway)) Serial.printf("[WIFI] Using fixed IP %s\n", STATIC_IP);
    else Serial.println("[WIFI] Could not set the fixed IP - using DHCP");
  }

  uint8_t bssid[6];
  bool pinned = strlen(WIFI_BSSID) > 0 &&
                sscanf(WIFI_BSSID, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                       &bssid[0], &bssid[1], &bssid[2], &bssid[3], &bssid[4], &bssid[5]) == 6;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 0, pinned ? bssid : nullptr);

  Serial.printf("[WIFI] Connecting to \"%s\" on %s", WIFI_SSID, USE_5GHZ ? "5 GHz" : "2.4 GHz");
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
      Serial.println("\n[WIFI] Still not connected. Check WIFI_SSID / WIFI_PASSWORD.");
      if (USE_5GHZ) {
        Serial.println("[WIFI] Does the router have a 5 GHz network with this name?");
        Serial.println("[WIFI] Some use a separate name for 5 GHz, like \"MyWiFi-5G\".");
      }
      Serial.println("[WIFI] Retrying...");
      start = millis();
    }
  }
  rememberAccessPoint();

  int channel = (int)WiFi.channel();
  Serial.println();
  Serial.printf("[WIFI] Connected. C5 IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Access point: %s (put this in WIFI_BSSID to always use it)\n",
                WiFi.BSSIDstr().c_str());
  Serial.printf("[WIFI] Signal from access point: %d dBm\n", (int)WiFi.RSSI());
  Serial.println("--------------------------------------------------");
  Serial.printf ("  BAND: %s    CHANNEL: %d\n", channel >= 36 ? "5 GHz" : "2.4 GHz", channel);
  if (channel >= 52 && channel <= 144) {
    Serial.println("  WARNING: DFS (radar) channel. The router may switch channels");
    Serial.println("  without warning. Fix its 5 GHz channel to 36-48 if you can.");
  }
  Serial.println("--------------------------------------------------");
}


/* ============================================================================
 *  SECTION 4: READING A TRANSMITTER'S PACKET COUNTER   (0 = not found)
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
 *  Keeps only:
 *    - transmitter boards (MAC 1a:00:00:00:00:xx)              -> node 1-254
 *    - the router's packets addressed to this C5 (ping replies) -> node 0
 *  Router beacons (sent to everyone, older packet type) are skipped.
 * ========================================================================== */
void onCsiReceived(void *ctx, wifi_csi_info_t *info) {
  if (info == nullptr || info->buf == nullptr) return;

  uint8_t  nodeId;
  uint32_t txSeq;
  if (CSI_FROM_TRANSMITTERS && memcmp(info->mac, nodePrefix, sizeof(nodePrefix)) == 0 &&
      info->mac[5] != 0 && info->mac[5] != 255) {
    nodeId = info->mac[5];
    txSeq  = findTxSeq(info->payload, info->payload_len, nodeId);
  } else if (CSI_FROM_ACCESS_POINT && apKnown &&
             memcmp(info->mac, apBssid, 6) == 0 && memcmp(info->dmac, myMac, 6) == 0) {
    nodeId = AP_NODE_ID;
    txSeq  = 0;
  } else {
    return;
  }

  static CsiQueueItem item;
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
  if (xQueueSend(csiQueue, &item, 0) != pdTRUE) queueFullDrops = queueFullDrops + 1;
}


/* ============================================================================
 *  SECTION 6: ESP-NOW CALLBACK  (counts transmitter packets heard)
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
  if (CSI_FROM_TRANSMITTERS) {
    check(esp_wifi_set_promiscuous(true), "Enable promiscuous mode");
    check(esp_now_init(), "Start ESP-NOW");
    check(esp_now_register_recv_cb(onEspNowReceived), "Register ESP-NOW callback");
    esp_now_peer_info_t peer = {};               // for beacons (2.4 GHz only)
    memcpy(peer.peer_addr, BROADCAST_ADDR, sizeof(BROADCAST_ADDR));
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    check(esp_now_add_peer(&peer), "Add broadcast peer");
  }

  // ESP32-C5 CSI settings (as Espressif's csi_recv_router), 8-bit values
  wifi_csi_config_t csiConfig = {};
  csiConfig.enable                   = true;
  csiConfig.acquire_csi_legacy       = true;
  csiConfig.acquire_csi_force_lltf   = false;
  csiConfig.acquire_csi_ht20         = true;   // ping replies + transmitters
  csiConfig.acquire_csi_ht40         = true;
  csiConfig.acquire_csi_vht          = false;  // 11ac / WiFi 6 are off (SECTION 3)
  csiConfig.acquire_csi_su           = false;
  csiConfig.acquire_csi_mu           = false;
  csiConfig.acquire_csi_dcm          = false;
  csiConfig.acquire_csi_beamformed   = false;
  csiConfig.acquire_csi_he_stbc_mode = 2;
  csiConfig.val_scale_cfg            = 0;
  csiConfig.dump_ack_en              = false;
  csiConfig.lltf_bit_mode            = 1;      // 8-bit, like the S3 and C6
  check(esp_wifi_set_csi_config(&csiConfig), "Set CSI config");
  check(esp_wifi_set_csi_rx_cb(onCsiReceived, nullptr), "Register CSI callback");
  check(esp_wifi_set_csi(true), "Enable CSI");

  Serial.printf("[CSI] Sources: %s%s\n",
                CSI_FROM_ACCESS_POINT ? "router (node 0)  " : "",
                CSI_FROM_TRANSMITTERS ? "transmitters 1a:00:00:00:00:xx" : "");
}

void sendBeacon() {
  if (!BEACONS_ON || WiFi.status() != WL_CONNECTED) return;
  rx_beacon_t beacon = {};
  beacon.magic       = RX_BEACON_MAGIC;
  beacon.version     = RX_BEACON_VERSION;
  beacon.receiver_id = RECEIVER_ID;
  beacon.channel     = (uint8_t)WiFi.channel();
  if (esp_now_send(BROADCAST_ADDR, (const uint8_t *)&beacon, sizeof(beacon)) == ESP_OK) beaconsSent++;
}


/* ============================================================================
 *  SECTION 8: PINGING THE ROUTER
 *  The pings only exist to make the router send packets back to us.
 *  Every reply = one CSI sample on the router -> C5 link. The target is
 *  read when pinging starts; a fixed IP keeps it valid across reconnects.
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
  pingCfg.count           = ESP_PING_COUNT_INFINITE;
  pingCfg.interval_ms     = 1000 / PING_RATE_HZ;
  pingCfg.timeout_ms      = 1000;
  pingCfg.data_size       = 1;
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
 *  SECTION 9: FINDING THE LAPTOP SERVER   (same as csi_receiver_24)
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
      }
    }
  }
  Serial.println("[SERVER] Waiting for the laptop server's announcement...");
  uint32_t start = millis();
  while (millis() - start < 4000) {
    checkDiscovery();
    if (strcmp(serverFoundBy, "announcement") == 0) return;
    delay(10);
  }
  if (serverKnown) {
    Serial.printf("[SERVER] Sending to %s (from %s)\n", serverAddress.toString().c_str(), serverFoundBy);
  } else {
    Serial.println("[SERVER] Not found yet. Is server.py running on a laptop on this WiFi?");
    Serial.println("[SERVER] (Keeps listening. If it never appears, set SERVER_IP in config.h.)");
  }
}


/* ============================================================================
 *  SECTION 10: UDP BATCHING AND SENDING
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
  if (batchSamples == 255) flushBatch();
}


/* ============================================================================
 *  SECTION 11: STATS PRINTER
 *  Healthy on 5 GHz: router CSI/s near PING_RATE_HZ, ping timeouts not rising.
 * ========================================================================== */
void printStats() {
  static uint32_t lastCsi[256];
  static uint32_t lastHeard[256];
  static uint32_t lastReplies = 0;
  const float seconds = STATS_PRINT_INTERVAL_MS / 1000.0f;
  int channel = (int)WiFi.channel();

  Serial.printf("\n[STATS] %s ch %d | WiFi %s | server %s (%s) | CSI %lu | sent %lu in %lu UDP"
                " | queue full %lu | UDP errors %lu | no-server drops %lu\n",
                channel >= 36 ? "5 GHz" : "2.4 GHz", channel,
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
                serverKnown ? serverAddress.toString().c_str() : "-", serverFoundBy,
                (unsigned long)totalCsi, (unsigned long)samplesSent, (unsigned long)udpPacketsSent,
                (unsigned long)queueFullDrops, (unsigned long)udpErrors, (unsigned long)noServerDrops);

  if (CSI_FROM_ACCESS_POINT) {
    uint32_t csi = nodeCsiCount[AP_NODE_ID], replies = pingReplies;
    Serial.printf("  router    | CSI/s %5.1f | pings ok/s %5.1f | RSSI %4d dBm | CSI %u bytes | timeouts %lu\n",
                  (csi - lastCsi[AP_NODE_ID]) / seconds, (replies - lastReplies) / seconds,
                  nodeLastRssi[AP_NODE_ID], nodeCsiLen[AP_NODE_ID], (unsigned long)pingTimeouts);
    lastCsi[AP_NODE_ID] = csi;
    lastReplies = replies;
  }
  if (CSI_FROM_TRANSMITTERS && !USE_5GHZ) {
    for (int node = 1; node < 255; node++) {
      uint32_t csi = nodeCsiCount[node], heard = nodeHeardCount[node];
      if (csi == 0 && heard == 0) continue;
      Serial.printf("  node %4d | CSI/s %5.1f | heard/s   %5.1f | RSSI %4d dBm | CSI %u bytes\n",
                    node, (csi - lastCsi[node]) / seconds, (heard - lastHeard[node]) / seconds,
                    nodeLastRssi[node], nodeCsiLen[node]);
      lastCsi[node] = csi;
      lastHeard[node] = heard;
    }
  }
}


/* ============================================================================
 *  SECTION 12: setup()
 * ========================================================================== */
void setup() {
  Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);       // Never stall when no Serial Monitor is open
#endif
  delay(1500);

  Serial.println("==================================================");
  Serial.printf ("  CSI receiver %d (ESP32-C5)  |  %s  |  home WiFi\n",
                 RECEIVER_ID, USE_5GHZ ? "5 GHz" : "2.4 GHz");
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
  csiInit();
  findServerAtStart();
  if (CSI_FROM_ACCESS_POINT) pingStart();
}


/* ============================================================================
 *  SECTION 13: loop()
 * ========================================================================== */
void loop() {
  static CsiQueueItem item;
  static uint32_t nextStatsMs  = millis() + STATS_PRINT_INTERVAL_MS;
  static uint32_t lastBeaconMs = 0;
  static uint32_t lastBlinkMs  = 0;
  static uint32_t lastTotal    = 0;
  static bool     ledOn        = true;
  static bool     wasConnected = true;

  // --- 1. Queue -> batch -> laptop ---
  if (xQueueReceive(csiQueue, &item, pdMS_TO_TICKS(5)) == pdTRUE) {
    do { addToBatch(item); } while (xQueueReceive(csiQueue, &item, 0) == pdTRUE);
  }
  if (batchSamples > 0 && millis() - batchStartMs >= BATCH_FLUSH_MS) flushBatch();

  // --- 2. After a reconnect: the C5 may now be on another mesh box ---
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  if (connectedNow && !wasConnected) {
    rememberAccessPoint();
    Serial.printf("[WIFI] Reconnected via %s, channel %d\n", WiFi.BSSIDstr().c_str(), (int)WiFi.channel());
  }
  wasConnected = connectedNow;

  // --- 3. Beacons (2.4 GHz mode only) and server announcements ---
  if (BEACONS_ON && millis() - lastBeaconMs >= 1000 / BEACON_RATE_HZ) {
    lastBeaconMs = millis();
    sendBeacon();
  }
  checkDiscovery();

  // --- 4. Status LED ---
  if (!connectedNow) {
    ledOn = false;
    ledSet(false);
  } else if (!serverKnown) {
    ledSet((millis() / 100) % 2);
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

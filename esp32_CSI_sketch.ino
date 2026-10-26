/**
 * @file esp32-csi-arduino.ino
 * @brief ESP32-S3 CSI Node — Arduino IDE sketch (ADR-018 compliant).
 *
 * Functional equivalent of the ESP-IDF firmware in
 * firmware/esp32-csi-node/ but buildable from the Arduino IDE.
 *
 * Capabilities:
 *   - WiFi STA connection with auto-reconnect
 *   - CSI frame capture in promiscuous mode
 *   - ADR-018 binary frame serialization
 *   - UDP streaming to aggregator host
 *   - Runtime provisioning via Serial commands
 *
 * Hardware:  ESP32-S3 (any board with 8 MB flash — DevKitC-1, XIAO, etc.)
 * Framework: Arduino-ESP32 v3.x (ESP-IDF v5.1+ under the hood)
 *
 * Quick start:
 *   1. Install Arduino IDE 2.x
 *   2. Add ESP32 board URL in Preferences (see README.md)
 *   3. Select board "ESP32S3 Dev Module"
 *   4. Edit the USER CONFIGURATION section below
 *   5. Upload
 *
 * @see firmware/esp32-csi-node/README.md  (full ESP-IDF version)
 * @see docs/adr/ADR-018-csi-binary-frame-format.md
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"

// ─── USER CONFIGURATION ────────────────────────────────────────────────────
// Edit these values for your deployment, or provision at runtime over Serial.

#define WIFI_SSID          "Cambukingham"
#define WIFI_PASSWORD      "gavril2006"                    // empty = open network
#define AGGREGATOR_IP      "172.21.240.1"       // host running sensing-server
#define AGGREGATOR_PORT    5005                  // UDP port
#define CSI_NODE_ID        1                     // unique per node (0-255)
#define CSI_WIFI_CHANNEL   6                     // channel to listen on
#define SAMPLE_RATE_HZ     20                    // target CSI frame rate

// ─── ADR-018 CONSTANTS ─────────────────────────────────────────────────────

static const uint32_t ADR018_MAGIC   = 0xC5110001;
static const uint16_t HEADER_SIZE    = 20;
static const uint16_t MAX_SUBCARRIERS = 128;    // Cap for I/Q buffer
static const int      WIFI_CONNECT_RETRIES     = 40;   // 40 * 500ms = 20s timeout
static const int      WIFI_CONNECT_RETRY_MS    = 500;

// ─── GLOBALS ────────────────────────────────────────────────────────────────

static WiFiUDP udp;
static IPAddress aggIP;
static uint32_t seqNum        = 0;
static bool     wifiConnected = false;
static unsigned long lastFrameMs = 0;
static const unsigned long frameIntervalMs = 1000 / SAMPLE_RATE_HZ;

// Runtime-configurable parameters (can be changed via Serial commands)
static char     cfgSsid[33]   = WIFI_SSID;
static char     cfgPass[65]   = WIFI_PASSWORD;
static char     cfgAggIp[16]  = AGGREGATOR_IP;
static uint16_t cfgAggPort    = AGGREGATOR_PORT;
static uint8_t  cfgNodeId     = CSI_NODE_ID;

// ─── ADR-018 PACKET BUFFER ─────────────────────────────────────────────────

// Maximum packet: 20-byte header + 64 subcarriers * 1 antenna * 2 bytes = 148
static uint8_t pktBuf[256];

// CSI callback data (written by WiFi task, read by loop())
static volatile bool     csiReady    = false;
static volatile int8_t   lastRssi    = -80;
static volatile int8_t   lastNoise   = -90;
static int8_t  iqBuffer[256];          // I/Q pairs from last callback
static volatile uint16_t iqLen       = 0;
static volatile uint8_t  csiAntennas = 1;
static volatile uint16_t csiSubcarriers = 0;

// ─── CSI CALLBACK ───────────────────────────────────────────────────────────

/**
 * Called by the WiFi driver on each received CSI frame.
 * Runs in the WiFi task context — keep it fast (no Serial, no UDP here).
 */
static void csi_callback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || info->len <= 0) return;

  lastRssi = info->rx_ctrl.rssi;
  lastNoise = info->rx_ctrl.noise_floor;
  csiAntennas = 1;  // ESP32-S3 single-antenna CSI by default

  // Each subcarrier produces 2 bytes (I, Q)
  uint16_t n_sc = info->len / 2;
  if (n_sc > MAX_SUBCARRIERS) n_sc = MAX_SUBCARRIERS;
  csiSubcarriers = n_sc;

  uint16_t copyLen = n_sc * 2;
  if (copyLen > sizeof(iqBuffer)) copyLen = sizeof(iqBuffer);
  memcpy(iqBuffer, info->buf, copyLen);
  iqLen = copyLen;

  csiReady = true;
}

// ─── ADR-018 PACKET BUILDER ────────────────────────────────────────────────

/**
 * Build an ADR-018 binary CSI frame in pktBuf.
 * @return total packet length in bytes
 */
static uint16_t buildAdr018Packet() {
  uint8_t  nAnt  = csiAntennas;
  uint16_t nSc   = csiSubcarriers;
  uint32_t freq  = 2412 + (CSI_WIFI_CHANNEL - 1) * 5;  // approximate MHz
  int8_t   rssi  = lastRssi;
  int8_t   noise = lastNoise;

  // Header (20 bytes, little-endian)
  uint16_t off = 0;
  memcpy(pktBuf + off, &ADR018_MAGIC, 4);   off += 4;  // magic
  pktBuf[off++] = cfgNodeId;                             // node_id
  pktBuf[off++] = nAnt;                                  // n_antennas
  pktBuf[off]   = (uint8_t)(nSc & 0xFF);                // n_subcarriers LE u16
  pktBuf[off+1] = (uint8_t)(nSc >> 8);
  off += 2;
  memcpy(pktBuf + off, &freq, 4);           off += 4;   // freq_mhz
  memcpy(pktBuf + off, &seqNum, 4);         off += 4;   // sequence
  pktBuf[off++] = (uint8_t)rssi;                         // rssi (i8)
  pktBuf[off++] = (uint8_t)noise;                        // noise (i8)
  pktBuf[off++] = 0;                                     // reserved
  pktBuf[off++] = 0;                                     // reserved

  // I/Q payload
  uint16_t iqBytes = nAnt * nSc * 2;
  if (iqBytes > sizeof(pktBuf) - HEADER_SIZE) {
    iqBytes = sizeof(pktBuf) - HEADER_SIZE;
  }
  if (iqLen > 0 && iqLen <= iqBytes) {
    memcpy(pktBuf + off, iqBuffer, iqLen);
    off += iqLen;
  } else if (iqBytes > 0) {
    memcpy(pktBuf + off, iqBuffer, iqBytes);
    off += iqBytes;
  }

  seqNum++;
  return off;
}

// ─── WIFI MANAGEMENT ────────────────────────────────────────────────────────

static void connectWiFi() {
  Serial.printf("[CSI] Connecting to WiFi SSID: %s\n", cfgSsid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid, cfgPass);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < WIFI_CONNECT_RETRIES) {
    delay(WIFI_CONNECT_RETRY_MS);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[CSI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifiConnected = false;
    Serial.println("\n[CSI] WiFi connection failed — will retry.");
  }
}

static void enableCSI() {
  // Enable promiscuous mode (required for CSI callbacks)
  esp_wifi_set_promiscuous(true);

  // Configure CSI collection
  wifi_csi_config_t csi_config = {
    .lltf_en           = true,
    .htltf_en          = true,
    .stbc_htltf2_en    = true,
    .ltf_merge_en      = true,
    .channel_filter_en = true,
    .manu_scale        = false,
    .shift             = false,
  };
  esp_wifi_set_csi_config(&csi_config);
  esp_wifi_set_csi_rx_cb(csi_callback, NULL);
  esp_wifi_set_csi(true);

  Serial.printf("[CSI] CSI enabled on channel %d (node %d)\n",
                CSI_WIFI_CHANNEL, cfgNodeId);
  Serial.printf("[CSI] Streaming to %s:%d (ADR-018 format)\n",
                cfgAggIp, cfgAggPort);
}

// ─── SERIAL COMMAND HANDLER ─────────────────────────────────────────────────

/**
 * Process runtime configuration commands from the Serial console.
 * Commands:
 *   ssid <name>        — set WiFi SSID
 *   pass <password>    — set WiFi password
 *   ip <a.b.c.d>       — set aggregator IP
 *   port <number>      — set aggregator UDP port
 *   node <0-255>       — set node ID
 *   reconnect          — reconnect WiFi with current settings
 *   status             — print current configuration
 *   help               — show available commands
 */
static void handleSerialCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.startsWith("ssid ")) {
    strncpy(cfgSsid, cmd.substring(5).c_str(), sizeof(cfgSsid) - 1);
    Serial.printf("[CFG] SSID set to: %s\n", cfgSsid);
  }
  else if (cmd.startsWith("pass ")) {
    strncpy(cfgPass, cmd.substring(5).c_str(), sizeof(cfgPass) - 1);
    Serial.println("[CFG] Password updated");
  }
  else if (cmd.startsWith("ip ")) {
    strncpy(cfgAggIp, cmd.substring(3).c_str(), sizeof(cfgAggIp) - 1);
    aggIP.fromString(cfgAggIp);
    Serial.printf("[CFG] Aggregator IP set to: %s\n", cfgAggIp);
  }
  else if (cmd.startsWith("port ")) {
    cfgAggPort = (uint16_t)cmd.substring(5).toInt();
    Serial.printf("[CFG] Aggregator port set to: %d\n", cfgAggPort);
  }
  else if (cmd.startsWith("node ")) {
    cfgNodeId = (uint8_t)cmd.substring(5).toInt();
    Serial.printf("[CFG] Node ID set to: %d\n", cfgNodeId);
  }
  else if (cmd == "reconnect") {
    Serial.println("[CFG] Reconnecting WiFi...");
    WiFi.disconnect();
    delay(1000);
    connectWiFi();
    enableCSI();
  }
  else if (cmd == "status") {
    Serial.println("──── ESP32-S3 CSI Node Status ────");
    Serial.printf("  SSID:       %s\n", cfgSsid);
    Serial.printf("  WiFi:       %s\n", wifiConnected ? "connected" : "disconnected");
    Serial.printf("  IP:         %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  Aggregator: %s:%d\n", cfgAggIp, cfgAggPort);
    Serial.printf("  Node ID:    %d\n", cfgNodeId);
    Serial.printf("  Frames:     %u\n", seqNum);
    Serial.printf("  Channel:    %d\n", CSI_WIFI_CHANNEL);
    Serial.println("──────────────────────────────────");
  }
  else if (cmd == "help") {
    Serial.println("Commands: ssid <n>, pass <p>, ip <a.b.c.d>, port <n>, "
                   "node <0-255>, reconnect, status, help");
  }
  else {
    Serial.printf("[CFG] Unknown command: %s (type 'help')\n", cmd.c_str());
  }
}

// ─── SETUP ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════╗");
  Serial.println("║  ESP32-S3 CSI Node — Arduino IDE (ADR-018)      ║");
  Serial.printf( "║  Node ID: %-3d    Channel: %-2d                    ║\n",
                 cfgNodeId, CSI_WIFI_CHANNEL);
  Serial.println("╚══════════════════════════════════════════════════╝");

  aggIP.fromString(cfgAggIp);

  connectWiFi();

  if (wifiConnected) {
    enableCSI();
    udp.begin(0);  // bind any local port for sending
  }

  Serial.println("[CSI] Type 'help' in Serial Monitor for runtime commands.");
}

// ─── MAIN LOOP ──────────────────────────────────────────────────────────────

void loop() {
  // Handle Serial commands
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleSerialCommand(line);
  }

  // Auto-reconnect WiFi if disconnected
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      wifiConnected = false;
      Serial.println("[CSI] WiFi disconnected — reconnecting...");
    }
    connectWiFi();
    if (wifiConnected) {
      enableCSI();
    }
    return;
  }

  // Rate-limit CSI frame transmission
  unsigned long now = millis();
  if (now - lastFrameMs < frameIntervalMs) return;
  lastFrameMs = now;

  // Send CSI frame if new data is available
  if (csiReady) {
    csiReady = false;

    uint16_t pktLen = buildAdr018Packet();

    udp.beginPacket(aggIP, cfgAggPort);
    udp.write(pktBuf, pktLen);
    udp.endPacket();

    // Print status every 100 frames
    if (seqNum % 100 == 0) {
      Serial.printf("[CSI] Frame %u | RSSI: %d dBm | %d subcarriers | -> %s:%d\n",
                    seqNum, (int)lastRssi, (int)csiSubcarriers,
                    cfgAggIp, cfgAggPort);
    }
  }
}
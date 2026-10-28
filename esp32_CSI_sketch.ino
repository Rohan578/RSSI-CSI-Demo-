/**
 * @file esp32-csi-arduino.ino
 * @brief ESP32-S3 CSI Node — Arduino IDE sketch (ADR-018 compliant).
 *
 * Functional equivalent of the ESP-IDF firmware in
 * firmware/esp32-csi-node/ but buildable from the Arduino IDE.
 *
 * Capabilities:
 *   - WiFi STA connection with auto-reconnect
 *   - Receives ESP-NOW packets from an ESP32-C6 and captures CSI from them
 *   - Learns the C6's MAC from its ESP-NOW packets and drops CSI from any
 *     other transmitter
 *   - ADR-018 binary frame serialization
 *   - UDP streaming to aggregator host (every captured frame, queued)
 *   - Runtime provisioning via Serial commands
 *
 * Data path:
 *   C6 --ESP-NOW (HT20 MCS0, broadcast)--> S3 --UDP over WiFi--> PC (csi_receiver.py)
 *
 * Both boards must join the same WiFi network so they share a channel.
 * The C6 sketch is esp32c6_espnow_sender.ino.
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
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ─── USER CONFIGURATION ────────────────────────────────────────────────────
// Edit these values for your deployment, or provision at runtime over Serial.

#define WIFI_SSID          "Cambukingham"
#define WIFI_PASSWORD      "gavril2006"                    // empty = open network
#define AGGREGATOR_IP      "172.21.240.1"       // the PC running csi_receiver.py
#define AGGREGATOR_PORT    5005                  // UDP port
#define CSI_NODE_ID        1                     // unique per node (0-255)

// Optional: pin the C6's MAC instead of learning it from its first ESP-NOW
// packet. Uncomment and fill in (the C6 prints its MAC on boot).
// #define TX_MAC          {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}

// ─── ADR-018 CONSTANTS ─────────────────────────────────────────────────────

static const uint32_t ADR018_MAGIC   = 0xC5110001;
static const uint16_t HEADER_SIZE    = 20;
// HT20 CSI with LLTF + HT-LTF merged is 128 subcarriers (256 bytes); packets
// with STBC add another 64 (384 bytes). Size the buffers for the larger case.
static const uint16_t MAX_SUBCARRIERS = 192;
static const uint16_t IQ_BUF_BYTES    = MAX_SUBCARRIERS * 2;
static const int      WIFI_CONNECT_RETRIES     = 40;   // 40 * 500ms = 20s timeout
static const int      WIFI_CONNECT_RETRY_MS    = 500;

// ─── ESP-NOW PAYLOAD (must match esp32c6_espnow_sender.ino) ────────────────

struct __attribute__((packed)) EspNowPayload {
  uint32_t magic;
  uint32_t seq;
  uint32_t uptime_ms;
};
static const uint32_t ESPNOW_MAGIC = 0xC6E50001;

// ─── GLOBALS ────────────────────────────────────────────────────────────────

static WiFiUDP udp;
static IPAddress aggIP;
static uint32_t seqNum        = 0;
static bool     wifiConnected = false;
static bool     espnowReady   = false;
static unsigned long lastStatMs = 0;

// Runtime-configurable parameters (can be changed via Serial commands)
static char     cfgSsid[33]   = WIFI_SSID;
static char     cfgPass[65]   = WIFI_PASSWORD;
static char     cfgAggIp[16]  = AGGREGATOR_IP;
static uint16_t cfgAggPort    = AGGREGATOR_PORT;
static uint8_t  cfgNodeId     = CSI_NODE_ID;

// ─── ADR-018 PACKET BUFFER ─────────────────────────────────────────────────

static uint8_t pktBuf[20 + IQ_BUF_BYTES];

// ─── CSI QUEUE ──────────────────────────────────────────────────────────────
// The CSI callback (WiFi task) pushes samples here; loop() pops them and sends
// UDP. A queue instead of one shared buffer means a burst from the C6 is not
// dropped just because loop() was busy.

struct CsiSample {
  int8_t   rssi;
  int8_t   noise;
  uint16_t len;                  // valid bytes in buf (always even)
  int8_t   buf[IQ_BUF_BYTES];    // (imag, real) int8 pairs
};

static const int CSI_QUEUE_DEPTH = 32;
static QueueHandle_t csiQueue = NULL;

// Counters written by WiFi-task callbacks, read by loop()
static volatile uint32_t csiQueued    = 0;   // samples accepted into the queue
static volatile uint32_t csiDropped   = 0;   // queue full
static volatile uint32_t csiFiltered  = 0;   // CSI from a MAC that is not the C6
static volatile uint32_t espnowRxCount = 0;  // valid ESP-NOW packets from the C6

// The C6's MAC. CSI frames from any other transmitter are discarded.
static uint8_t txMac[6]        = {0};
static volatile bool txMacKnown    = false;
static volatile bool txMacAnnounce = false;  // loop() prints the MAC once

// ─── ESP-NOW RECEIVE CALLBACK ───────────────────────────────────────────────

/**
 * Called by the WiFi driver for each ESP-NOW packet. Used only to learn the
 * C6's MAC (the CSI itself comes from csi_callback). No Serial here.
 */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len != (int)sizeof(EspNowPayload)) return;

  EspNowPayload pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != ESPNOW_MAGIC) return;

  espnowRxCount++;

#ifndef TX_MAC
  if (!txMacKnown || memcmp(txMac, info->src_addr, 6) != 0) {
    memcpy(txMac, info->src_addr, 6);
    txMacKnown = true;
    txMacAnnounce = true;
  }
#endif
}

// ─── CSI CALLBACK ───────────────────────────────────────────────────────────

/**
 * Called by the WiFi driver on each received CSI frame.
 * Runs in the WiFi task context — keep it fast (no Serial, no UDP here).
 */
static void csi_callback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || info->len <= 0) return;

  // Keep only CSI measured from the C6's transmissions.
  if (!txMacKnown || memcmp(info->mac, txMac, 6) != 0) {
    csiFiltered++;
    return;
  }

  CsiSample s;
  s.rssi  = info->rx_ctrl.rssi;
  s.noise = info->rx_ctrl.noise_floor;

  // Each subcarrier produces 2 bytes (imag, real)
  uint16_t len = info->len;
  if (len > IQ_BUF_BYTES) len = IQ_BUF_BYTES;
  len &= ~1;
  s.len = len;
  memcpy(s.buf, info->buf, len);

  if (xQueueSend(csiQueue, &s, 0) == pdTRUE) {
    csiQueued++;
  } else {
    csiDropped++;
  }
}

// ─── ADR-018 PACKET BUILDER ────────────────────────────────────────────────

/**
 * Build an ADR-018 binary CSI frame in pktBuf.
 * @return total packet length in bytes
 */
static uint16_t buildAdr018Packet(int8_t rssi, int8_t noise, const int8_t *iq, uint16_t iqLen) {
  uint8_t  nAnt  = 1;                       // ESP32-S3 single-antenna CSI
  uint16_t nSc   = iqLen / 2;
  uint32_t freq  = 2412 + (WiFi.channel() - 1) * 5;  // MHz, from the real channel

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
  memcpy(pktBuf + off, iq, iqLen);
  off += iqLen;

  seqNum++;
  return off;
}

// ─── WIFI MANAGEMENT ────────────────────────────────────────────────────────

static void connectWiFi() {
  Serial.printf("[CSI] Connecting to WiFi SSID: %s\n", cfgSsid);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // modem sleep makes the radio miss ESP-NOW frames
  WiFi.begin(cfgSsid, cfgPass);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < WIFI_CONNECT_RETRIES) {
    delay(WIFI_CONNECT_RETRY_MS);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[CSI] Connected! IP: %s  channel: %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    wifiConnected = false;
    Serial.println("\n[CSI] WiFi connection failed — will retry.");
  }
}

static void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void enableEspNow() {
  if (espnowReady) return;   // stays initialised across WiFi reconnects
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ERROR: esp_now_init() failed");
    return;
  }
  esp_now_register_recv_cb(espnow_recv_cb);
  espnowReady = true;
  Serial.println("[ESPNOW] Listening for the C6");
}

static void enableCSI() {
  // Promiscuous mode is kept on so CSI is delivered for every frame the radio
  // hears; csi_callback() then discards everything that is not from the C6.
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

  enableEspNow();

  Serial.printf("[CSI] CSI enabled on channel %d (node %d)\n",
                WiFi.channel(), cfgNodeId);
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
    Serial.printf("  Channel:    %d\n", WiFi.channel());
    Serial.printf("  Aggregator: %s:%d\n", cfgAggIp, cfgAggPort);
    Serial.printf("  Node ID:    %d\n", cfgNodeId);
    Serial.printf("  Frames sent:%u\n", seqNum);
    Serial.print ("  C6 MAC:     ");
    if (txMacKnown) printMac(txMac); else Serial.print("(not seen yet)");
    Serial.println();
    Serial.printf("  ESP-NOW rx: %u packets from the C6\n", (unsigned)espnowRxCount);
    Serial.printf("  CSI:        %u queued, %u dropped (queue full), %u from other MACs\n",
                  (unsigned)csiQueued, (unsigned)csiDropped, (unsigned)csiFiltered);
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
  Serial.printf( "║  Node ID: %-3d    Link: C6 -> S3 via ESP-NOW      ║\n",
                 cfgNodeId);
  Serial.println("╚══════════════════════════════════════════════════╝");

  aggIP.fromString(cfgAggIp);

  csiQueue = xQueueCreate(CSI_QUEUE_DEPTH, sizeof(CsiSample));
  if (!csiQueue) {
    Serial.println("[CSI] FATAL: could not allocate CSI queue");
    while (true) delay(1000);
  }

#ifdef TX_MAC
  const uint8_t pinned[6] = TX_MAC;
  memcpy(txMac, pinned, 6);
  txMacKnown = true;
  Serial.print("[ESPNOW] Using pinned C6 MAC ");
  printMac(txMac);
  Serial.println();
#endif

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

  if (txMacAnnounce) {
    txMacAnnounce = false;
    Serial.print("[ESPNOW] Learned C6 MAC ");
    printMac(txMac);
    Serial.println();
  }

  // Drain queued CSI samples and forward each one to the PC. The cap keeps
  // Serial commands and reconnects responsive if the queue is full.
  CsiSample s;
  for (int i = 0; i < CSI_QUEUE_DEPTH && xQueueReceive(csiQueue, &s, 0) == pdTRUE; i++) {
    uint16_t pktLen = buildAdr018Packet(s.rssi, s.noise, s.buf, s.len);

    udp.beginPacket(aggIP, cfgAggPort);
    udp.write(pktBuf, pktLen);
    udp.endPacket();
  }

  // Print a status line every 5 s
  unsigned long now = millis();
  if (now - lastStatMs >= 5000) {
    lastStatMs = now;
    Serial.printf("[CSI] sent %u | queue drops %u | ignored (other MACs) %u | ESP-NOW rx %u\n",
                  seqNum, (unsigned)csiDropped, (unsigned)csiFiltered,
                  (unsigned)espnowRxCount);
    if (!txMacKnown) {
      Serial.println("[CSI] No packets from the C6 yet — check that it is running "
                     "and on the same WiFi network.");
    }
  }
}

/*
 * ============================================================================
 *  ESP32-C6 SuperMini  -  ESP-NOW CSI Transmitter
 * ============================================================================
 *
 *  WHAT THIS DOES
 *    1. Connects to the same WiFi network as the ESP32-S3 (this puts both
 *       boards on the same radio channel, which ESP-NOW requires).
 *    2. Broadcasts a tiny ESP-NOW packet every SEND_INTERVAL_MS (default 100 Hz).
 *    3. Reconnects automatically if WiFi drops.
 *
 *  The packet contents do not matter. The S3 measures CSI from the radio
 *  frame itself, so this board is just a steady, known transmitter.
 *
 *  IMPORTANT - PHY RATE
 *    ESP-NOW defaults to 1 Mbps 802.11b, which has no OFDM training fields
 *    and therefore produces no usable CSI. setupEspNow() forces HT20 MCS0.
 *
 *  HARDWARE
 *    - ESP32-C6 SuperMini (any ESP32-C6 board should work)
 *
 *  ARDUINO IDE SETUP
 *    - Install "esp32 by Espressif Systems" (version 3.x or newer) in
 *      Boards Manager. Version 3.x is required for ESP32-C6 support.
 *    - Board:              "ESP32C6 Dev Module"
 *    - USB CDC On Boot:    "Enabled"   (otherwise Serial Monitor shows nothing)
 *    - Serial Monitor:     115200 baud
 *
 *  NOTE: The ESP32-C6 only supports 2.4 GHz WiFi. It cannot join a
 *        5 GHz-only network.
 * ============================================================================
 */

#include <WiFi.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_idf_version.h"


// ============================================================================
//  SECTION 1: USER SETTINGS  -  Change these to match your setup
// ============================================================================

// --- WiFi credentials ---
// Must be the SAME network the ESP32-S3 joins.
const char* WIFI_SSID     = "ORBI21";
const char* WIFI_PASSWORD = "yellowsun456";

// --- Timing ---
const unsigned long SEND_INTERVAL_MS     = 10;     // 10 ms = ~100 packets/s
const unsigned long STATS_INTERVAL_MS    = 1000;   // How often to print a stats line
const unsigned long WIFI_TIMEOUT_MS      = 15000;  // Give up a connect attempt after this
const unsigned long WIFI_RETRY_DELAY_MS  = 5000;   // Wait before retrying a failed connect

// --- Status LED ---
// Most ESP32-C6 SuperMini boards have a plain LED on GPIO 15.
// If yours is different, change this number. Set to -1 to disable the LED.
const int  STATUS_LED_PIN       = 15;
const bool STATUS_LED_ACTIVE_LOW = false;   // Set true if your LED lights on LOW


// ============================================================================
//  SECTION 2: GLOBAL OBJECTS & STATE  -  Usually no need to change
// ============================================================================

// Packet sent over ESP-NOW. The S3 sketch has an identical copy of this
// struct and uses `magic` to recognise the transmitter - keep them in sync.
struct __attribute__((packed)) EspNowPayload {
  uint32_t magic;       // ESPNOW_MAGIC
  uint32_t seq;         // Packet counter since boot
  uint32_t uptime_ms;   // millis() at send time
};
static const uint32_t ESPNOW_MAGIC = 0xC6E50001;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool espnowReady = false;
unsigned long lastSendTime  = 0;     // Last time (in ms) a packet was sent
unsigned long lastStatsTime = 0;
uint32_t packetCounter      = 0;     // Packets handed to ESP-NOW since boot
uint32_t sendErrors         = 0;     // esp_now_send() calls that returned an error
uint32_t lastStatsCounter   = 0;


// ============================================================================
//  SECTION 3: HELPER FUNCTIONS
// ============================================================================

// ---------------------------------------------------------------------------
//  setLed()  -  Turns the status LED on or off (handles active-low boards)
// ---------------------------------------------------------------------------
void setLed(bool on) {
  if (STATUS_LED_PIN < 0) return;
  bool level = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(STATUS_LED_PIN, level ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
//  connectToWiFi()  -  Tries to join the WiFi network.
//  Returns true if connected, false if it timed out.
// ---------------------------------------------------------------------------
bool connectToWiFi() {
  Serial.printf("\n[WiFi] Connecting to \"%s\" ...\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);              // Station mode = normal WiFi client
  WiFi.setSleep(false);             // Modem sleep would make timing uneven
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > WIFI_TIMEOUT_MS) {
      Serial.println("[WiFi] Connection timed out.");
      WiFi.disconnect();
      return false;
    }
    if (STATUS_LED_PIN >= 0) setLed(!digitalRead(STATUS_LED_PIN));   // Blink while connecting
    Serial.print(".");
    delay(250);
  }

  setLed(true);   // Solid LED = connected
  Serial.println();
  Serial.println("[WiFi] Connected!");
  Serial.print  ("[WiFi] Board IP address: ");
  Serial.println(WiFi.localIP());
  Serial.printf ("[WiFi] Channel:          %d\n", WiFi.channel());
  Serial.printf ("[WiFi] Signal strength:  %d dBm\n", WiFi.RSSI());
  Serial.print  ("[WiFi] Board MAC:        ");
  Serial.println(WiFi.macAddress());
  return true;
}

// ---------------------------------------------------------------------------
//  ensureWiFiConnected()  -  Call this in loop(). If WiFi dropped, it keeps
//  retrying until the connection is back.
// ---------------------------------------------------------------------------
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  setLed(false);
  Serial.println("[WiFi] Not connected. Reconnecting...");
  while (!connectToWiFi()) {
    Serial.printf("[WiFi] Retrying in %lu ms...\n", WIFI_RETRY_DELAY_MS);
    delay(WIFI_RETRY_DELAY_MS);
  }
}

// ---------------------------------------------------------------------------
//  setupEspNow()  -  Starts ESP-NOW, adds the broadcast peer and forces the
//  HT20 MCS0 PHY rate so the S3 gets real CSI from each packet.
//  Call once, after WiFi is connected. Returns true on success.
// ---------------------------------------------------------------------------
bool setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ERROR: esp_now_init() failed");
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;                 // 0 = follow the channel the board is on
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ESPNOW] ERROR: could not add broadcast peer");
    return false;
  }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  esp_now_rate_config_t rate = {};
  rate.phymode = WIFI_PHY_MODE_HT20;
  rate.rate    = WIFI_PHY_RATE_MCS0_LGI;
  rate.ersu    = false;
  rate.dcm     = false;
  esp_err_t err = esp_now_set_peer_rate_config(BROADCAST_MAC, &rate);
#else
  esp_err_t err = esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_MCS0_LGI);
#endif
  if (err != ESP_OK) {
    Serial.printf("[ESPNOW] WARNING: could not set HT20 MCS0 rate (err 0x%x). "
                  "The S3 will probably see no CSI.\n", (unsigned)err);
  }

  Serial.println("[ESPNOW] Ready - broadcasting at HT20 MCS0");
  return true;
}

// ---------------------------------------------------------------------------
//  sendEspNowPacket()  -  Broadcasts one packet. Returns true if ESP-NOW
//  accepted it. Broadcast has no acknowledgement, so success only means it
//  was queued for transmission, not that the S3 heard it.
// ---------------------------------------------------------------------------
bool sendEspNowPacket() {
  EspNowPayload pkt;
  pkt.magic     = ESPNOW_MAGIC;
  pkt.seq       = packetCounter;
  pkt.uptime_ms = millis();
  return esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, sizeof(pkt)) == ESP_OK;
}


// ============================================================================
//  SECTION 4: setup()  -  Runs once at power-on / reset
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1500);   // Give the USB serial port a moment to come up

  Serial.println("\n=========================================");
  Serial.println("  ESP32-C6 ESP-NOW CSI sender - starting up");
  Serial.println("=========================================");

  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    setLed(false);
  }

  ensureWiFiConnected();
  espnowReady = setupEspNow();

  Serial.printf("[ESPNOW] Sending every %lu ms\n", SEND_INTERVAL_MS);
}


// ============================================================================
//  SECTION 5: loop()  -  Runs over and over forever
// ============================================================================
void loop() {
  ensureWiFiConnected();   // Reconnect if WiFi dropped

  if (!espnowReady) {      // Retry a failed ESP-NOW setup every few seconds
    delay(WIFI_RETRY_DELAY_MS);
    espnowReady = setupEspNow();
    return;
  }

  // Send a packet once every SEND_INTERVAL_MS (non-blocking timer)
  if (millis() - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = millis();
    if (sendEspNowPacket()) {
      packetCounter++;
    } else {
      sendErrors++;
    }
  }

  // Print a one-line stats summary every STATS_INTERVAL_MS
  if (millis() - lastStatsTime >= STATS_INTERVAL_MS) {
    uint32_t sent = packetCounter - lastStatsCounter;
    lastStatsCounter = packetCounter;
    lastStatsTime = millis();
    Serial.printf("[ESPNOW] %lu pkt/s | total %lu | errors %lu | ch %d | rssi %d dBm\n",
                  (unsigned long)(sent * 1000UL / STATS_INTERVAL_MS),
                  (unsigned long)packetCounter, (unsigned long)sendErrors,
                  WiFi.channel(), WiFi.RSSI());
  }
}

/*
 * ============================================================================
 *  CSI TRANSMITTER NODE  -  Arduino IDE version
 *  Works on: ESP32 (DevKit / WROOM-32), ESP32-C6 SuperMini, ESP32-C3
 * ============================================================================
 *
 *  WHAT THIS BOARD DOES
 *    It sends a small radio packet SEND_RATE_HZ times per second, and
 *    nothing else. It never joins the WiFi network.
 *
 *    The ESP32-S3 receiver hears each packet and measures how the signal
 *    changed on its way across the room (CSI). People standing, sitting,
 *    walking or falling between this board and the S3 change that signal.
 *    That's what the AI model learns from.
 *
 *  HOW THE PACKETS ARE SENT: ESP-NOW
 *    ESP-NOW is Espressif's direct board-to-board protocol. The packet goes
 *    straight from this board to the S3 through the air, NOT through the
 *    WiFi access point. That matters: if it went through the access point,
 *    the S3 would be measuring the access point's signal instead of this
 *    board's path across the room.
 *
 *  BASED ON
 *    Espressif's official "csi_send" example
 *    (github.com/espressif/esp-csi, examples/get-started/csi_send).
 *
 *  ARDUINO IDE SETUP
 *    - Boards Manager: "esp32 by Espressif Systems", version 3.3.0 or newer
 *    - Tools > Board:  "ESP32 Dev Module"   for an ESP32 DevKit
 *                      "ESP32C6 Dev Module" for the C6 SuperMini
 *                      "ESP32C3 Dev Module" for a C3 SuperMini
 *    - C6 / C3 SuperMini only: Tools > USB CDC On Boot > "Enabled"
 *      (otherwise the Serial Monitor shows nothing)
 *    - Serial Monitor: 115200 baud
 *
 *  FILES (tabs at the top of the Arduino IDE)
 *    config.h          <- NODE_ID, channel, send rate, power, LED. EDIT THIS.
 *    espnow_packet.h   <- what's inside each packet (receiver must match)
 *    csi_transmitter   <- this file
 *
 *  SECTIONS IN THIS FILE
 *    1. Includes and counters
 *    2. Status LED
 *    3. Radio setup (no WiFi connection - just tune the radio)
 *    4. Send-finished callback
 *    5. ESP-NOW setup
 *    6. Sending one packet
 *    7. Stats printer
 *    8. setup()  - runs once
 *    9. loop()   - runs forever: send on schedule, print stats
 * ============================================================================
 */


/* ============================================================================
 *  SECTION 1: INCLUDES AND COUNTERS
 * ========================================================================== */
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_idf_version.h>

#include "config.h"
#include "espnow_packet.h"

// ESP-NOW "broadcast" address: every board on the channel can hear it.
// The S3 picks out our packets by our MAC address (see SECTION 3).
static const uint8_t BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// This board's MAC address, set in SECTION 3
static uint8_t myMac[6];

// Counters, printed every few seconds (SECTION 7).
// The two "air" counters are updated by the WiFi driver in the background,
// so they're marked "volatile" (always re-read from memory).
static uint32_t countQueued  = 0;           // Packets handed to the radio
static volatile uint32_t countAirOk   = 0;  // Radio reports: packet went out
static volatile uint32_t countAirFail = 0;  // Radio reports: packet failed
static uint32_t countBusy    = 0;           // Radio was busy, packet skipped
static uint32_t countErrors  = 0;           // Any other send error

static uint32_t packetSeq = 0;              // Counts up by 1 per packet

// Catch impossible settings when compiling, instead of on the board
static_assert(NODE_ID >= 1 && NODE_ID <= 254, "NODE_ID must be between 1 and 254");
static_assert(WIFI_CHANNEL >= 1 && WIFI_CHANNEL <= 13, "WIFI_CHANNEL must be 1-13");
static_assert(SEND_RATE_HZ >= 1 && SEND_RATE_HZ <= 200, "SEND_RATE_HZ must be 1-200");
static_assert(TX_POWER_DBM >= 2 && TX_POWER_DBM <= 20, "TX_POWER_DBM must be 2-20");


/* ============================================================================
 *  SECTION 2: STATUS LED
 *  Blinks once per second while sending. Not blinking = not sending.
 * ========================================================================== */
void ledInit() {
  if (STATUS_LED_GPIO < 0) return;
  pinMode(STATUS_LED_GPIO, OUTPUT);
}

void ledSet(bool on) {
  if (STATUS_LED_GPIO < 0) return;
  bool level = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(STATUS_LED_GPIO, level ? HIGH : LOW);
}


/* ============================================================================
 *  SECTION 3: RADIO SETUP
 *  We turn the WiFi radio on but never connect to a network. We only:
 *    - give the board a predictable MAC address
 *    - lock it to the access point's channel at 20 MHz width
 *    - fix the transmit power so every node is equally loud
 *    - turn off power saving so the radio never sleeps
 *  "check()" stops with a clear message if any step fails.
 * ========================================================================== */
void check(esp_err_t err, const char *what) {
  if (err == ESP_OK) return;
  Serial.printf("[ERROR] %s failed: %s\n", what, esp_err_to_name(err));
  Serial.println("[ERROR] Stopped. Fix the problem above and press RESET.");
  while (true) delay(1000);
}

void radioInit() {
  WiFi.mode(WIFI_STA);        // Turn the radio on in "station" mode...
  WiFi.disconnect();          // ...but don't connect to any network
  WiFi.setSleep(false);       // No power saving: radio stays fully awake

  // --- MAC address: 1a:00:00:00:00:<NODE_ID> --------------------------------
  // Every board gets a MAC that contains its node number, so the S3
  // receiver (and you, reading logs) can tell nodes apart at a glance.
  // 0x1a marks it as a "locally assigned" address, so it can't clash with
  // any real device's MAC.
  const uint8_t mac[6] = { 0x1a, 0x00, 0x00, 0x00, 0x00, NODE_ID };
  memcpy(myMac, mac, sizeof(mac));
  check(esp_wifi_set_mac(WIFI_IF_STA, myMac), "Set MAC address");

  // --- WiFi standard and channel width ---------------------------------------
  // 802.11 b/g/n only. The C6 also supports WiFi 6 (11ax), but the
  // original ESP32 and C3 don't, so it's turned off to make every node
  // send the same kind of signal. 20 MHz width matches the access point.
  check(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N), "Set WiFi protocol");
  check(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20), "Set 20 MHz bandwidth");

  // --- Channel ------------------------------------------------------------------
  // Must be the SAME channel the access point (and so the S3) uses,
  // otherwise the S3 never hears us.
  check(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE), "Set channel");

  // --- Fixed transmit power -------------------------------------------------
  // The driver takes power in units of 0.25 dBm, so 15 dBm -> 60.
  // The radio rounds DOWN to the nearest step it supports.
  check(esp_wifi_set_max_tx_power(TX_POWER_DBM * 4), "Set TX power");

  int8_t actualPower = 0;
  esp_wifi_get_max_tx_power(&actualPower);
  Serial.printf("[RADIO] Channel %d, 20 MHz, TX power %.2f dBm\n",
                WIFI_CHANNEL, actualPower / 4.0f);
}


/* ============================================================================
 *  SECTION 4: SEND-FINISHED CALLBACK
 *  The WiFi driver calls this after each packet leaves the radio.
 *  It runs inside the WiFi driver, so it only updates counters.
 *
 *  Note: broadcast packets are never acknowledged by the receiver, so
 *  "ok" here means "it went out over the air", not "the S3 got it".
 *  The Pi dashboard's lost-packet count shows real delivery.
 *
 *  (The function's shape changed in Arduino core 3.3 / ESP-IDF 5.5,
 *   so both versions are handled.)
 * ========================================================================== */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSendDone(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
void onSendDone(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) {
    countAirOk = countAirOk + 1;
  } else {
    countAirFail = countAirFail + 1;
  }
}


/* ============================================================================
 *  SECTION 5: ESP-NOW SETUP
 * ========================================================================== */
void espnowInit() {
  check(esp_now_init(), "Start ESP-NOW");
  check(esp_now_register_send_cb(onSendDone), "Register send callback");

  // "Peer" = who we send to. We use the broadcast address so we don't
  // need to know the S3's MAC address. No encryption: nothing in these
  // packets is secret, and encryption would add airtime.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, sizeof(BROADCAST_ADDR));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  check(esp_now_add_peer(&peer), "Add broadcast peer");

  // --- Packet speed (data rate) -----------------------------------------------
  // Every node must use the SAME rate so the S3 always measures the same
  // kind of signal. 802.11n, 20 MHz, MCS0 (the slowest, most robust
  // 802.11n rate) gives the S3 detailed CSI and reaches the furthest.
  // Espressif's csi_send example uses MCS0 as well.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  esp_now_rate_config_t rateCfg = {};
  rateCfg.phymode = WIFI_PHY_MODE_HT20;
  rateCfg.rate    = WIFI_PHY_RATE_MCS0_LGI;
  rateCfg.ersu    = false;
  rateCfg.dcm     = false;
  check(esp_now_set_peer_rate_config(BROADCAST_ADDR, &rateCfg), "Set ESP-NOW rate");
#else
  // Older Arduino cores (before 3.2) use this older function instead
  check(esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_MCS0_LGI), "Set ESP-NOW rate");
#endif

  Serial.println("[ESPNOW] Broadcasting at 802.11n MCS0, 20 MHz");
}


/* ============================================================================
 *  SECTION 6: SENDING ONE PACKET
 *  >>> To change WHAT is sent, edit espnow_packet.h and the lines that
 *  >>> fill in "packet" below (and update the S3 receiver to match).
 * ========================================================================== */
void sendOnePacket() {
  tx_packet_t packet;
  packet.magic      = TX_PACKET_MAGIC;
  packet.version    = TX_PACKET_VERSION;
  packet.node_id    = NODE_ID;
  packet.seq        = ++packetSeq;
  packet.tx_time_us = micros();

  esp_err_t err = esp_now_send(BROADCAST_ADDR, (const uint8_t *)&packet, sizeof(packet));
  if (err == ESP_OK) {
    countQueued++;
  } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
    // The radio's send queue is full - usually a very busy channel.
    // Skip this packet; the next one goes out on schedule.
    countBusy++;
  } else {
    countErrors++;
  }

  // Blink: toggle the LED twice per second = one blink per second
  static bool ledOn = false;
  const uint32_t halfSecond = (SEND_RATE_HZ / 2 > 0) ? SEND_RATE_HZ / 2 : 1;
  if (packetSeq % halfSecond == 0) {
    ledOn = !ledOn;
    ledSet(ledOn);
  }
}


/* ============================================================================
 *  SECTION 7: STATS PRINTER
 *  Healthy board: "pkt/s" close to SEND_RATE_HZ, "busy" and "errors" at 0.
 * ========================================================================== */
void printStats() {
  static uint32_t lastQueued = 0;
  uint32_t queued = countQueued;
  float rate = (queued - lastQueued) * 1000.0f / STATS_PRINT_INTERVAL_MS;
  lastQueued = queued;

  Serial.printf("node %d | pkt/s %.1f | sent %lu | air ok %lu | air fail %lu | busy %lu | errors %lu\n",
                NODE_ID, rate, (unsigned long)queued, (unsigned long)countAirOk,
                (unsigned long)countAirFail, (unsigned long)countBusy,
                (unsigned long)countErrors);
}


/* ============================================================================
 *  SECTION 8: setup()  -  runs once at power-on / reset
 * ========================================================================== */
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1500);   // Give the USB serial port a moment to come up

  ledInit();
  ledSet(false);

  radioInit();
  espnowInit();

  Serial.println("==================================================");
  Serial.println("  CSI transmitter");
  Serial.printf ("  Node ID  : %d\n", NODE_ID);
  Serial.printf ("  MAC      : %02x:%02x:%02x:%02x:%02x:%02x\n",
                 myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf ("  Channel  : %d (must match the access point)\n", WIFI_CHANNEL);
  Serial.printf ("  Rate     : %d packets per second\n", SEND_RATE_HZ);
  Serial.println("==================================================");
}


/* ============================================================================
 *  SECTION 9: loop()  -  runs over and over forever
 *  Sends on an exact schedule using micros(): each packet is due exactly
 *  1/SEND_RATE_HZ seconds after the previous one, so the rate stays steady
 *  even if printing or a slow send delays one loop.
 * ========================================================================== */
void loop() {
  static const uint32_t periodUs  = 1000000UL / SEND_RATE_HZ;
  static uint32_t nextSendUs      = micros();
  static uint32_t nextStatsMs     = millis() + STATS_PRINT_INTERVAL_MS;

  // --- Time to send? ---
  if ((int32_t)(micros() - nextSendUs) >= 0) {
    sendOnePacket();
    nextSendUs += periodUs;

    // If we fell far behind (e.g. something blocked for a while),
    // restart the schedule instead of sending a burst to catch up.
    if ((int32_t)(micros() - nextSendUs) > (int32_t)(periodUs * 5)) {
      nextSendUs = micros() + periodUs;
    }
  }

  // --- Time to print stats? ---
  if ((int32_t)(millis() - nextStatsMs) >= 0) {
    printStats();
    nextStatsMs += STATS_PRINT_INTERVAL_MS;
  }

  delay(1);   // Let the WiFi driver and other background tasks run
}

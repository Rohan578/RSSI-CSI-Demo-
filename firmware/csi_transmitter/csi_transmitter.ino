/*
 * ============================================================================
 *  CSI TRANSMITTER NODE  -  home WiFi version, 2.4 GHz, Arduino IDE
 *  Works on: ESP32 (DevKit / WROOM-32), ESP32-C3, ESP32-C6 SuperMini, ESP32-C5
 * ============================================================================
 *
 *  WHAT THIS BOARD DOES
 *    Sends a tiny ESP-NOW packet SEND_RATE_HZ times per second. The 2.4 GHz
 *    receiver measures how each packet's radio signal changed on its way
 *    across the room (CSI). People in the room change that signal; the
 *    server on the laptop learns from it.
 *
 *    The board never joins your WiFi (it needs no password). It only has to
 *    send on the same channel as the receiver, which IS joined to the WiFi.
 *
 *  WHAT'S NEW IN THIS VERSION (vs. flash-ready)
 *    - Follows the receiver's channel. The receiver broadcasts a small
 *      beacon; the transmitter locks onto the channel where it hears it,
 *      and searches again if the beacons stop. Home routers change their
 *      2.4 GHz channel on their own - before, the transmitters then went
 *      silent until someone power-cycled them.
 *    - Random timing jitter (+/- SEND_JITTER_US) and a start offset per
 *      NODE_ID, so up to 10 transmitters don't keep colliding in the air.
 *    - 2.4 GHz only (the 5 GHz data comes from the C5 receiver + router).
 *
 *  BASED ON
 *    Espressif's "csi_send" example (github.com/espressif/esp-csi) and the
 *    team's flash-ready transmitter.
 *
 *  ARDUINO IDE SETUP
 *    - Boards Manager: "esp32 by Espressif Systems", 3.3.0 or newer
 *    - Tools > Board: "ESP32 Dev Module" / "ESP32C3 Dev Module" /
 *                     "ESP32C6 Dev Module" / "ESP32C5 Dev Module"
 *    - C3 / C6 / C5: Tools > USB CDC On Boot > "Enabled"
 *    - Serial Monitor: 115200 baud
 *
 *  FILES (tabs in the Arduino IDE)
 *    config.h          <- NODE_ID (and the fallback channel). EDIT THIS.
 *    espnow_packet.h   <- packet layouts (identical in all three sketches)
 *    csi_transmitter   <- this file
 *
 *  SECTIONS
 *    1. Includes, state, counters
 *    2. Status LED
 *    3. Radio setup
 *    4. ESP-NOW (send + receive callbacks, setup)
 *    5. Finding the channel (receiver beacons, then fallbacks)
 *    6. Sending one packet
 *    7. Stats printer
 *    8. setup()
 *    9. loop()
 * ============================================================================
 */


/* ============================================================================
 *  SECTION 1: INCLUDES, STATE, COUNTERS
 * ========================================================================== */
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_idf_version.h>

#include "config.h"
#include "espnow_packet.h"

static const uint8_t BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t myMac[6];

// Channel state
enum ChannelMode { MODE_SEARCHING, MODE_LOCKED, MODE_FALLBACK };
static ChannelMode mode = MODE_SEARCHING;
static uint8_t  activeChannel = WIFI_CHANNEL;   // The channel we're on now

// Written by the ESP-NOW receive callback (WiFi task), read by loop()
static volatile uint32_t lastBeaconMs   = 0;    // When a beacon was last heard
static volatile uint8_t  beaconChannel  = 0;    // Channel it said it's on
static volatile uint32_t beaconsHeard   = 0;
static volatile uint32_t receiversSeen  = 0;    // Bit n = receiver id n heard (1..31)

// Counters for the stats line
static uint32_t countQueued  = 0;               // Packets handed to the radio
static volatile uint32_t countAirOk   = 0;      // Radio: packet went out
static volatile uint32_t countAirFail = 0;      // Radio: packet failed
static uint32_t countBusy    = 0;               // Radio busy, packet skipped
static uint32_t countErrors  = 0;               // Other send errors
static uint32_t channelSwitches = 0;
static uint32_t packetSeq    = 0;

// Catch impossible settings when compiling, not on the board
static_assert(NODE_ID >= 1 && NODE_ID <= 254, "NODE_ID must be between 1 and 254");
static_assert(WIFI_CHANNEL >= 1 && WIFI_CHANNEL <= 13, "WIFI_CHANNEL must be 1-13 (2.4 GHz)");
static_assert(SEND_RATE_HZ >= 1 && SEND_RATE_HZ <= 200, "SEND_RATE_HZ must be 1-200");
static_assert(TX_POWER_DBM >= 2 && TX_POWER_DBM <= 20, "TX_POWER_DBM must be 2-20");
static_assert(SEND_JITTER_US >= 0 && SEND_JITTER_US < 1000000 / SEND_RATE_HZ / 2,
              "SEND_JITTER_US must be less than half the send period");


/* ============================================================================
 *  SECTION 2: STATUS LED   (patterns are listed in config.h, section 6)
 * ========================================================================== */
static const int ledPin = STATUS_LED_GPIO;   // -1 = no LED

void ledInit() {
  if (ledPin < 0) return;
  pinMode(ledPin, OUTPUT);
}

void ledSet(bool on) {
  if (ledPin < 0) return;
  bool level = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(ledPin, level ? HIGH : LOW);
}

// Called often from loop(); works out the pattern from the time
void ledUpdate() {
  uint32_t t = millis();
  bool on;
  if (mode == MODE_SEARCHING) {
    on = (t / 100) % 2;                        // fast: 5 blinks/s
  } else if (mode == MODE_FALLBACK) {
    uint32_t p = t % 1000;                     // double blink every second
    on = (p < 80) || (p >= 200 && p < 280);
  } else {
    on = (t % 1000) < 500;                     // slow: 1 blink/s
  }
  ledSet(on);
}


/* ============================================================================
 *  SECTION 3: RADIO SETUP
 *  The radio is turned on but never joins a network:
 *    - 2.4 GHz only (matters on the dual-band C5)
 *    - MAC address 1a:00:00:00:00:<NODE_ID>
 *    - 802.11 b/g/n at 20 MHz, fixed power, no power saving
 * ========================================================================== */
void check(esp_err_t err, const char *what) {
  if (err == ESP_OK) return;
  Serial.printf("[ERROR] %s failed: %s\n", what, esp_err_to_name(err));
  Serial.println("[ERROR] Stopped. Fix the problem above and press RESET.");
  while (true) delay(1000);
}

void setChannel(uint8_t channel) {
  if (channel < 1 || channel > 13) return;
  check(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), "Set channel");
  if (channel != activeChannel) channelSwitches++;
  activeChannel = channel;
}

void radioInit() {
  WiFi.mode(WIFI_STA);          // Radio on in "station" mode...
  WiFi.disconnect();            // ...connected to nothing
  WiFi.setSleep(false);         // No power saving: never miss a slot

#if CONFIG_SOC_WIFI_SUPPORT_5G
  // Dual-band C5: stay on 2.4 GHz. (Must come after WiFi.mode().)
  if (!WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY)) check(ESP_FAIL, "Set 2.4 GHz band");
#endif

  // MAC 1a:00:00:00:00:<NODE_ID>. 0x1a = "locally assigned", so it can
  // never clash with a real device. The receiver recognises us by it.
  const uint8_t mac[6] = { 0x1a, 0x00, 0x00, 0x00, 0x00, NODE_ID };
  memcpy(myMac, mac, sizeof(mac));
  check(esp_wifi_set_mac(WIFI_IF_STA, myMac), "Set MAC address");

  // 802.11n (and the older b/g it builds on), 20 MHz. WiFi 6 is off so the
  // ESP32, C3, C6 and C5 all send exactly the same kind of signal.
#if CONFIG_SOC_WIFI_SUPPORT_5G
  wifi_protocols_t protocols = {};
  protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
  check(esp_wifi_set_protocols(WIFI_IF_STA, &protocols), "Set WiFi protocol");
  wifi_bandwidths_t widths = {};
  widths.ghz_2g = WIFI_BW_HT20;
  widths.ghz_5g = WIFI_BW_HT20;
  check(esp_wifi_set_bandwidths(WIFI_IF_STA, &widths), "Set 20 MHz bandwidth");
#else
  check(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N), "Set WiFi protocol");
  check(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20), "Set 20 MHz bandwidth");
#endif

  // Fixed power, in units of 0.25 dBm (15 dBm -> 60). Same on every node.
  check(esp_wifi_set_max_tx_power(TX_POWER_DBM * 4), "Set TX power");
  int8_t actualPower = 0;
  esp_wifi_get_max_tx_power(&actualPower);
  Serial.printf("[RADIO] 2.4 GHz, 20 MHz, TX power %.2f dBm\n", actualPower / 4.0f);
}


/* ============================================================================
 *  SECTION 4: ESP-NOW
 *  Send callback: counts packets that left the radio. Broadcasts are never
 *    acknowledged, so "ok" means "sent", not "received" - the server's
 *    lost-packet count shows real delivery.
 *  Receive callback: notices receiver beacons. Runs in the WiFi task, so it
 *    only stores a few numbers; loop() does the rest.
 *  (Both callback shapes changed in Arduino core 3.3 / ESP-IDF 5.5.)
 * ========================================================================== */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSendDone(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
void onSendDone(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) countAirOk = countAirOk + 1;
  else                                countAirFail = countAirFail + 1;
}

void onEspNowReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (data == nullptr || len != (int)sizeof(rx_beacon_t)) return;
  rx_beacon_t beacon;
  memcpy(&beacon, data, sizeof(beacon));
  if (beacon.magic != RX_BEACON_MAGIC || beacon.version != RX_BEACON_VERSION) return;
  lastBeaconMs  = millis();
  beaconChannel = beacon.channel;
  beaconsHeard  = beaconsHeard + 1;
  if (beacon.receiver_id < 32) receiversSeen = receiversSeen | (1UL << beacon.receiver_id);
}

void espnowInit() {
  check(esp_now_init(), "Start ESP-NOW");
  check(esp_now_register_send_cb(onSendDone), "Register send callback");
  check(esp_now_register_recv_cb(onEspNowReceived), "Register receive callback");

  // Broadcast peer. channel 0 = "whatever channel the radio is on now",
  // so it keeps working when we switch channels.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, sizeof(BROADCAST_ADDR));
  peer.channel = 0;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  check(esp_now_add_peer(&peer), "Add broadcast peer");

  // Same data rate on every node, so the receiver always measures the same
  // packet type: 802.11n MCS0, 20 MHz (slowest, most robust; as Espressif).
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  esp_now_rate_config_t rateCfg = {};
  rateCfg.phymode = WIFI_PHY_MODE_HT20;
  rateCfg.rate    = WIFI_PHY_RATE_MCS0_LGI;
  rateCfg.ersu    = false;
  rateCfg.dcm     = false;
  check(esp_now_set_peer_rate_config(BROADCAST_ADDR, &rateCfg), "Set ESP-NOW rate");
#else
  check(esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_MCS0_LGI), "Set ESP-NOW rate");
#endif
}


/* ============================================================================
 *  SECTION 5: FINDING THE CHANNEL
 *  searchForReceiver(): listen on each channel for BEACON_DWELL_MS until a
 *    receiver beacon is heard. Tries `first` first (normally the channel we
 *    were on), then 1..13. Returns the channel, or 0 if none was heard.
 *  fallbackChannel(): no receiver heard - copy the home WiFi's channel
 *    (AUTO_CHANNEL_SSID), else WIFI_CHANNEL.
 * ========================================================================== */
bool beaconSince(uint32_t t) {
  return beaconsHeard > 0 && (int32_t)(lastBeaconMs - t) >= 0;
}

uint8_t searchForReceiver(uint8_t first) {
  uint8_t order[14];
  int n = 0;
  if (first >= 1 && first <= 13) order[n++] = first;
  for (uint8_t ch = 1; ch <= 13; ch++) if (ch != first) order[n++] = ch;

  for (int i = 0; i < n; i++) {
    setChannel(order[i]);
    uint32_t start = millis();
    while (millis() - start < BEACON_DWELL_MS) {
      if (beaconSince(start) && beaconChannel == order[i]) return order[i];
      ledUpdate();
      delay(5);
    }
  }
  return 0;
}

uint8_t fallbackChannel() {
  const char *ssid = AUTO_CHANNEL_SSID;
  if (strlen(ssid) > 0) {
    Serial.printf("[CHANNEL] Scanning for \"%s\" to copy its 2.4 GHz channel...\n", ssid);
    int16_t count = WiFi.scanNetworks(false, true);
    int bestRssi = -1000;
    uint8_t best = 0;
    for (int16_t i = 0; i < count; i++) {
      int ch = WiFi.channel(i);
      if (WiFi.SSID(i) == ssid && ch >= 1 && ch <= 13 && WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        best = (uint8_t)ch;
      }
    }
    WiFi.scanDelete();
    if (best) {
      Serial.printf("[CHANNEL] Found \"%s\" on channel %d (%d dBm)\n", ssid, best, bestRssi);
      return best;
    }
    Serial.printf("[CHANNEL] \"%s\" not found\n", ssid);
  }
  Serial.printf("[CHANNEL] Using WIFI_CHANNEL %d from config.h\n", WIFI_CHANNEL);
  return WIFI_CHANNEL;
}

// Runs the whole search and leaves the radio on the chosen channel.
void acquireChannel(uint8_t first) {
  mode = MODE_SEARCHING;
  if (FOLLOW_RECEIVER) {
    Serial.println("[CHANNEL] Listening for a receiver beacon on each channel...");
    uint8_t ch = searchForReceiver(first);
    if (ch) {
      setChannel(ch);
      mode = MODE_LOCKED;
      Serial.printf("[CHANNEL] Receiver heard on channel %d - sending there\n", ch);
      return;
    }
    Serial.println("[CHANNEL] No receiver heard (is it on and connected to the WiFi?)");
  }
  setChannel(fallbackChannel());
  mode = FOLLOW_RECEIVER ? MODE_FALLBACK : MODE_LOCKED;
}


/* ============================================================================
 *  SECTION 6: SENDING ONE PACKET
 * ========================================================================== */
void sendOnePacket() {
  tx_packet_t packet;
  packet.magic      = TX_PACKET_MAGIC;
  packet.version    = TX_PACKET_VERSION;
  packet.node_id    = NODE_ID;
  packet.seq        = ++packetSeq;
  packet.tx_time_us = micros();

  esp_err_t err = esp_now_send(BROADCAST_ADDR, (const uint8_t *)&packet, sizeof(packet));
  if (err == ESP_OK)                     countQueued++;
  else if (err == ESP_ERR_ESPNOW_NO_MEM) countBusy++;     // channel very busy: skip one
  else                                   countErrors++;
}


/* ============================================================================
 *  SECTION 7: STATS PRINTER
 *  Healthy: "LOCKED", pkt/s near SEND_RATE_HZ, beacon age < 1 s, busy 0.
 * ========================================================================== */
void printStats() {
  static uint32_t lastQueued = 0;
  uint32_t queued = countQueued;
  float rate = (queued - lastQueued) * 1000.0f / STATS_PRINT_INTERVAL_MS;
  lastQueued = queued;

  const char *m = mode == MODE_LOCKED ? "LOCKED" : mode == MODE_FALLBACK ? "FALLBACK" : "SEARCH";
  char rx[64] = "";
  uint32_t seen = receiversSeen;
  receiversSeen = 0;
  for (int i = 1; i < 32; i++) {
    if (seen & (1UL << i)) {
      char part[8];
      snprintf(part, sizeof(part), "%s%d", rx[0] ? "," : "", i);
      strncat(rx, part, sizeof(rx) - strlen(rx) - 1);
    }
  }
  long beaconAge = beaconsHeard ? (long)(millis() - lastBeaconMs) : -1;

  Serial.printf("node %d | %s ch %d | pkt/s %.1f | receivers [%s] | beacon age %ld ms"
                " | busy %lu | errors %lu | channel changes %lu\n",
                NODE_ID, m, activeChannel, rate, rx, beaconAge,
                (unsigned long)countBusy, (unsigned long)countErrors,
                (unsigned long)channelSwitches);
}


/* ============================================================================
 *  SECTION 8: setup()
 * ========================================================================== */
void setup() {
  Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);       // Never stall when no Serial Monitor is open
#endif
  delay(1500);

  ledInit();
  ledSet(false);

  Serial.println("==================================================");
  Serial.println("  CSI transmitter (home WiFi, 2.4 GHz)");
  Serial.printf ("  Node ID  : %d\n", NODE_ID);
  Serial.printf ("  Rate     : %d packets/s, jitter +/- %d us\n", SEND_RATE_HZ, SEND_JITTER_US);
  Serial.println("==================================================");

  radioInit();
  espnowInit();
  Serial.printf("[RADIO] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  acquireChannel(WIFI_CHANNEL);
}


/* ============================================================================
 *  SECTION 9: loop()
 *    - send on schedule (period + jitter), starting at an offset per NODE_ID
 *    - LOCKED:   beacons stopped for BEACON_LOST_MS -> search again
 *    - FALLBACK: every SEARCH_RETRY_MS, look for a receiver again
 * ========================================================================== */
void loop() {
  static const uint32_t periodUs = 1000000UL / SEND_RATE_HZ;
  // Spread the nodes over the period so they don't all start together
  static uint32_t nextSendUs   = micros() + (NODE_ID % 11) * (periodUs / 11);
  static uint32_t nextStatsMs  = millis() + STATS_PRINT_INTERVAL_MS;
  static uint32_t lockedSinceMs = millis();
  static uint32_t lastSearchMs  = millis();

  // --- 1. Send when due ---
  if ((int32_t)(micros() - nextSendUs) >= 0) {
    sendOnePacket();
    int32_t jitter = 0;
    if (SEND_JITTER_US > 0) {
      jitter = (int32_t)(esp_random() % (2 * SEND_JITTER_US + 1)) - SEND_JITTER_US;
    }
    nextSendUs += periodUs + jitter;
    if ((int32_t)(micros() - nextSendUs) > (int32_t)(periodUs * 5)) {
      nextSendUs = micros() + periodUs;      // fell far behind: restart schedule
    }
  }

  // --- 2. Keep the channel right ---
  uint32_t now = millis();
  if (mode == MODE_LOCKED && FOLLOW_RECEIVER) {
    if (beaconSince(lockedSinceMs)) {
      // A beacon on our channel saying the receiver moved? Follow it.
      uint8_t ch = beaconChannel;
      if (ch >= 1 && ch <= 13 && ch != activeChannel) {
        Serial.printf("[CHANNEL] Receiver moved to channel %d - following\n", ch);
        setChannel(ch);
      }
    }
    uint32_t reference = beaconSince(lockedSinceMs) ? lastBeaconMs : lockedSinceMs;
    if (now - reference > BEACON_LOST_MS) {
      Serial.printf("[CHANNEL] No receiver beacon for %d s - searching again\n", BEACON_LOST_MS / 1000);
      acquireChannel(activeChannel);
      lockedSinceMs = millis();
      lastSearchMs = millis();
    }
  } else if (mode == MODE_FALLBACK) {
    if (beaconSince(lastSearchMs)) {
      uint8_t ch = beaconChannel;            // receiver came up (near) our channel
      if (ch >= 1 && ch <= 13) setChannel(ch);
      mode = MODE_LOCKED;
      lockedSinceMs = millis();
      Serial.printf("[CHANNEL] Receiver heard on channel %d - locked\n", activeChannel);
    } else if (now - lastSearchMs > SEARCH_RETRY_MS) {
      acquireChannel(activeChannel);
      lockedSinceMs = millis();
      lastSearchMs = millis();
    }
  }

  // --- 3. LED and stats ---
  ledUpdate();
  if ((int32_t)(now - nextStatsMs) >= 0) {
    printStats();
    nextStatsMs += STATS_PRINT_INTERVAL_MS;
  }

  delay(1);   // Let the WiFi driver run
}

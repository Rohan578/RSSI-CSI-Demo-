/*
 * ============================================================================
 *  config.h  -  SETTINGS for a CSI transmitter (home WiFi version, 2.4 GHz)
 *               Works on: ESP32, ESP32-C3, ESP32-C6 (SuperMini), ESP32-C5
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 *
 *  !!! EVERY TRANSMITTER NEEDS ITS OWN NODE_ID (1 to 10). !!!
 *  Label each board with its number and write down where it sits.
 *  Everything else can stay the same on every transmitter.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. Node ID  (1 to 10 for this setup; the code accepts 1-254)
 *     Becomes the last byte of the board's MAC address:
 *     NODE_ID 3  ->  MAC 1a:00:00:00:00:03
 *     Must match a "transmitters" entry in server/room.json.
 * ------------------------------------------------------------------------- */
#define NODE_ID                 1

/* ---------------------------------------------------------------------------
 *  2. Finding the right channel  (normally nothing to change here)
 *
 *  The transmitter must send on the SAME 2.4 GHz channel the receiver is on.
 *  The receiver is connected to your home WiFi, so that is the router's
 *  2.4 GHz channel - and home routers change it by themselves.
 *
 *  FOLLOW_RECEIVER 1 (recommended): the 2.4 GHz receiver broadcasts a tiny
 *    "I'm on channel N" beacon 5 times a second. At power-up the transmitter
 *    listens on each channel until it hears one, locks onto it, and starts
 *    sending. If the beacons stop for BEACON_LOST_MS (router changed
 *    channel, receiver rebooted...) it searches again by itself.
 *
 *  If no receiver is heard (e.g. it's still booting), it falls back to:
 *    AUTO_CHANNEL_SSID  - your home WiFi's name: copy its 2.4 GHz channel
 *                         (the board never joins it, so no password needed)
 *    WIFI_CHANNEL       - used if that network isn't found either
 *  ...and keeps looking for receiver beacons every SEARCH_RETRY_MS.
 * ------------------------------------------------------------------------- */
#define FOLLOW_RECEIVER         1
#define AUTO_CHANNEL_SSID       "ORBI21"
#define WIFI_CHANNEL            1        /* ORBI21's 2.4 GHz channel on 2026-11-06 */

#define BEACON_DWELL_MS         450      /* Time spent listening on each channel */
#define BEACON_LOST_MS          10000    /* No beacon this long -> search again */
#define SEARCH_RETRY_MS         30000    /* In fallback, look for beacons this often */

/* ---------------------------------------------------------------------------
 *  3. Packets per second  (same value on EVERY transmitter)
 *     50 = good detail for posture / movement.
 *     With 8-10 transmitters on a busy home network, 40 is a bit gentler.
 * ------------------------------------------------------------------------- */
#define SEND_RATE_HZ            50

/* ---------------------------------------------------------------------------
 *  4. Timing jitter  (microseconds)
 *     Each packet is sent up to this much early or late, at random.
 *     Transmitters run on separate clocks; without jitter two of them can
 *     drift into the same time slot and collide for seconds at a time.
 *     2000 us = +/- 2 ms. 0 = off.
 * ------------------------------------------------------------------------- */
#define SEND_JITTER_US          2000

/* ---------------------------------------------------------------------------
 *  5. Transmit power in dBm  (2 to 20, same value on EVERY transmitter)
 *     15 is a level every ESP32 chip can reach reliably.
 * ------------------------------------------------------------------------- */
#define TX_POWER_DBM            15

/* ---------------------------------------------------------------------------
 *  6. Status LED
 *     slow blink (1/s)  = sending, receiver heard
 *     fast blink (5/s)  = searching for the receiver's channel
 *     double blink      = sending on the fallback channel (no receiver heard)
 *
 *     Picked automatically from the board chosen in Tools > Board:
 *       ESP32 Dev Module   -> GPIO 2  (blue LED on most DevKits)
 *       ESP32C6 Dev Module -> GPIO 15 (plain LED on the C6 SuperMini)
 *       ESP32C3 Dev Module -> GPIO 8  (C3 SuperMini, lights when LOW)
 *       ESP32C5 Dev Module -> the built-in RGB LED (GPIO 27 on the C5 DevKit)
 *     To force a pin, replace the block with your own two #defines.
 *     -1 = no LED. If it's ON when it should be OFF, flip STATUS_LED_ACTIVE_LOW.
 * ------------------------------------------------------------------------- */
#if CONFIG_IDF_TARGET_ESP32C6
  #define STATUS_LED_GPIO       15
  #define STATUS_LED_ACTIVE_LOW 0
#elif CONFIG_IDF_TARGET_ESP32C3
  #define STATUS_LED_GPIO       8
  #define STATUS_LED_ACTIVE_LOW 1
#elif CONFIG_IDF_TARGET_ESP32
  #define STATUS_LED_GPIO       2
  #define STATUS_LED_ACTIVE_LOW 0
#elif CONFIG_IDF_TARGET_ESP32C5 && defined(LED_BUILTIN)
  #define STATUS_LED_GPIO       LED_BUILTIN
  #define STATUS_LED_ACTIVE_LOW 0
#else
  #define STATUS_LED_GPIO       -1
  #define STATUS_LED_ACTIVE_LOW 0
#endif

/* ---------------------------------------------------------------------------
 *  7. Serial Monitor (115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000

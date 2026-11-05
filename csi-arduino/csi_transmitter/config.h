/*
 * ============================================================================
 *  config.h  -  SETTINGS for the CSI transmitter
 *               (works on ESP32, ESP32-C6 SuperMini, ESP32-C3 and ESP32-C5)
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 *
 *  !!! EVERY BOARD NEEDS ITS OWN NODE_ID. !!!
 *  Write down where each node is placed in the room.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. Node ID  (1 to 254, different on every transmitter)
 *     Also becomes the last byte of the board's MAC address:
 *     NODE_ID 3  ->  MAC 1a:00:00:00:00:03
 *
 *     Tip: give each chip type its own range so logs are easy to read,
 *     e.g. ESP32 = 1-19, C6 = 20-39, C3 = 40-59, C5 = 60-79.
 * ------------------------------------------------------------------------- */
#define NODE_ID                 1       /* This setup: the five ESP32s are 1, 2, 3, 4, 5 */

/* ---------------------------------------------------------------------------
 *  2. WiFi band
 *     0 = 2.4 GHz  (every chip; this is what the S3 receiver uses)
 *     1 = 5 GHz    (ESP32-C5 ONLY - the other chips have no 5 GHz radio)
 *     A 5 GHz transmitter is only heard by a C5 receiver that is also on
 *     5 GHz, on the same channel.
 * ------------------------------------------------------------------------- */
#define USE_5GHZ                0

/* ---------------------------------------------------------------------------
 *  3a. Find the channel automatically (recommended)
 *     At power-up the transmitter scans for this network name and copies
 *     its channel on this board's band (strongest signal wins), so you
 *     don't have to re-flash when the access point's channel changes.
 *     Just power-cycle the transmitters after changing it.
 *     It keeps trying for AUTO_CHANNEL_TIMEOUT_MS, then uses WIFI_CHANNEL.
 *     Set to "" to always use WIFI_CHANNEL.
 *     (The transmitter never joins the network, so it needs no password.)
 * ------------------------------------------------------------------------- */
#define AUTO_CHANNEL_SSID       "linksys222"
#define AUTO_CHANNEL_TIMEOUT_MS 30000

/* ---------------------------------------------------------------------------
 *  3b. WiFi channel (fallback if the network above isn't found)
 *     MUST match the channel your access point is fixed to, on that band.
 *     The receiver prints the access point's channel when it starts.
 *       2.4 GHz: 1 to 13          (1, 6 or 11 are best)
 *       5 GHz:   36, 40, 44, 48   (best: no radar rules)
 *                or 149, 153, 157, 161, 165
 *                (52-144 are "DFS" channels: the access point may jump
 *                 away from them without warning - avoid for sensing)
 * ------------------------------------------------------------------------- */
#define WIFI_CHANNEL            11      /* linksys222 2.4 GHz channel as of 2026-11-04 */

/* ---------------------------------------------------------------------------
 *  4. Packets per second
 *     More = finer detail in time, but more airtime used.
 *     50 is a good start with several nodes. Use the SAME value on every node.
 * ------------------------------------------------------------------------- */
#define SEND_RATE_HZ            50

/* ---------------------------------------------------------------------------
 *  5. Transmit power in dBm  (2 to 20)
 *     Use the SAME value on every node so they're equally loud.
 *     15 is a level every ESP32 chip can reach reliably.
 * ------------------------------------------------------------------------- */
#define TX_POWER_DBM            15

/* ---------------------------------------------------------------------------
 *  6. Status LED  (blinks once per second while sending)
 *     Picked automatically from the board you select in Tools > Board:
 *       ESP32 Dev Module   -> GPIO 2  (blue LED on most DevKits)
 *       ESP32C6 Dev Module -> GPIO 15 (plain LED on the C6 SuperMini)
 *       ESP32C3 Dev Module -> GPIO 8  (C3 SuperMini, lights when LOW)
 *       ESP32C5 Dev Module -> the board's built-in RGB LED (GPIO 27 on the
 *                             ESP32-C5 DevKit), lights white
 *     To force a pin, replace the whole block with two lines, e.g.:
 *       #define STATUS_LED_GPIO        15
 *       #define STATUS_LED_ACTIVE_LOW  0
 *     Use -1 to turn the LED off. If it's ON when it should be OFF,
 *     flip STATUS_LED_ACTIVE_LOW.
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
 *  7. Serial Monitor (set it to 115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000     /* How often counters print */

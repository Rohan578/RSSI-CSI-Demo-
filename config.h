/*
 * ============================================================================
 *  config.h  -  SETTINGS for the CSI transmitter
 *               (works on ESP32, ESP32-C6 SuperMini and ESP32-C3)
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
 *     e.g. ESP32 = 1-19, C6 = 20-39, C3 = 40-59.
 * ------------------------------------------------------------------------- */
#define NODE_ID                 2

/* ---------------------------------------------------------------------------
 *  2. WiFi channel  (1 to 13)
 *     MUST match the channel your access point is fixed to.
 *     The S3 receiver prints the access point's channel when it starts -
 *     use that number here, on every transmitter.
 * ------------------------------------------------------------------------- */
#define WIFI_CHANNEL            11

/* ---------------------------------------------------------------------------
 *  3. Packets per second
 *     More = finer detail in time, but more airtime used.
 *     50 is a good start with several nodes. Use the SAME value on every node.
 * ------------------------------------------------------------------------- */
#define SEND_RATE_HZ            50

/* ---------------------------------------------------------------------------
 *  4. Transmit power in dBm  (2 to 20)
 *     Use the SAME value on every node so they're equally loud.
 *     15 is a level every ESP32 chip can reach reliably.
 * ------------------------------------------------------------------------- */
#define TX_POWER_DBM            15

/* ---------------------------------------------------------------------------
 *  5. Status LED  (blinks once per second while sending)
 *     Picked automatically from the board you select in Tools > Board:
 *       ESP32 Dev Module   -> GPIO 2  (blue LED on most DevKits)
 *       ESP32C6 Dev Module -> GPIO 15 (plain LED on the C6 SuperMini)
 *       ESP32C3 Dev Module -> GPIO 8  (C3 SuperMini, lights when LOW)
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
#else
  #define STATUS_LED_GPIO       -1
  #define STATUS_LED_ACTIVE_LOW 0
#endif

/* ---------------------------------------------------------------------------
 *  6. Serial Monitor (set it to 115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000     /* How often counters print */

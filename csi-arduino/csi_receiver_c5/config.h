/*
 * ============================================================================
 *  config.h  -  SETTINGS for the ESP32-C5 dual-band CSI receiver
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. Band  -  the C5 listens on ONE band at a time
 *     0 = 2.4 GHz      1 = 5 GHz
 *     To compare bands at the same moment, run this C5 on 5 GHz and the
 *     S3 receiver on 2.4 GHz together. The Pi logs both.
 * ------------------------------------------------------------------------- */
#define USE_5GHZ                1

/* ---------------------------------------------------------------------------
 *  2. WiFi access point
 *     WPA2, on a FIXED channel (not "auto"), 20 MHz wide.
 *     5 GHz: use channel 36, 40, 44, 48 or 149-165. Avoid 52-144 ("DFS"):
 *     the access point may jump off those channels without warning.
 *     If your access point uses a different name for its 5 GHz network
 *     (e.g. "MyWiFi-5G"), put that name here when USE_5GHZ is 1.
 * ------------------------------------------------------------------------- */
#define WIFI_SSID               "ORBI21"
#define WIFI_PASSWORD           "__WIFI_PASSWORD__"

/* ---------------------------------------------------------------------------
 *  3. Raspberry Pi (where the CSI data goes)
 *     The Pi's IP address. Find it on the Pi with:  hostname -I
 * ------------------------------------------------------------------------- */
#define PI_SERVER_IP            "172.16.0.75"   /* the Pi on ORBI21 (linksys222: "192.168.1.50") */
#define PI_SERVER_PORT          5005        /* Must match UDP_PORT in server.py */

/* ---------------------------------------------------------------------------
 *  4. Receiver ID  (1 to 255)
 *     The S3 is receiver 1, so this C5 is receiver 2 by default.
 * ------------------------------------------------------------------------- */
#define RECEIVER_ID             2

/* ---------------------------------------------------------------------------
 *  5. Where the CSI comes from  (1 = on, 0 = off)
 *
 *  CSI_FROM_ACCESS_POINT
 *     The C5 pings the access point PING_RATE_HZ times per second and
 *     measures CSI on every reply. The link measured is access point -> C5.
 *     Works on BOTH bands and needs no extra hardware. This is the main
 *     source on 5 GHz, because the ESP32 / C6 / C3 transmitters are
 *     2.4 GHz only. Shows up on the Pi as "node 0" (= access point).
 *
 *  CSI_FROM_TRANSMITTERS
 *     Measures CSI on ESP-NOW packets from the transmitter boards
 *     (MAC 1a:00:00:00:00:xx), exactly like the S3. On 5 GHz only C5
 *     transmitters (USE_5GHZ 1, same channel) can be heard.
 * ------------------------------------------------------------------------- */
#define CSI_FROM_ACCESS_POINT   1
#define CSI_FROM_TRANSMITTERS   1

#define PING_RATE_HZ            50          /* Pings to the access point per second */
#define PING_TARGET_IP          ""          /* "" = the gateway (ORBI21's router).
                                               linksys222 has no router: use "192.168.1.50" (the Pi). */

/* ---------------------------------------------------------------------------
 *  6. Transmitter MAC prefix (must match the transmitter sketch)
 * ------------------------------------------------------------------------- */
#define NODE_MAC_PREFIX         { 0x1a, 0x00, 0x00, 0x00, 0x00 }

/* ---------------------------------------------------------------------------
 *  7. Batching (how CSI samples are packed into UDP packets to the Pi)
 * ------------------------------------------------------------------------- */
#define MAX_UDP_PAYLOAD         1400        /* Bytes per UDP packet */
#define BATCH_FLUSH_MS          50          /* Max time a sample waits */
#define CSI_QUEUE_LENGTH        64          /* Samples buffered between tasks */

/* ---------------------------------------------------------------------------
 *  8. Status LED
 *     -2 = the board's built-in LED (ESP32-C5 DevKit: RGB LED on GPIO 27,
 *          lights white). Any other number = that GPIO. -1 = no LED.
 *     off = no WiFi | solid = WiFi OK, no CSI yet | blinking = working
 * ------------------------------------------------------------------------- */
#define STATUS_LED_GPIO         -2
#define STATUS_LED_ACTIVE_LOW   0

/* ---------------------------------------------------------------------------
 *  9. Serial Monitor (set it to 115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000

/*
 * ============================================================================
 *  config.h  -  SETTINGS for the ESP32-S3 CSI receiver
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. WiFi access point
 *     Must be 2.4 GHz, WPA2, on a FIXED channel (not "auto").
 *     When the S3 connects it prints the channel - set WIFI_CHANNEL on
 *     every transmitter to that number.
 * ------------------------------------------------------------------------- */
#define WIFI_SSID               "ORBI21"
#define WIFI_PASSWORD           "yellowsun456"

/* ---------------------------------------------------------------------------
 *  2. Raspberry Pi (where the CSI data goes)
 *     The Pi's IP address on the access point's network. Find it on the
 *     Pi with:  hostname -I
 *     Tip: in the access point's settings, give the Pi a "DHCP reservation"
 *     (a fixed IP) so this never changes.
 * ------------------------------------------------------------------------- */
#define PI_SERVER_IP            "192.168.1.50"
#define PI_SERVER_PORT          5005        /* Must match UDP_PORT in server.py */

/* ---------------------------------------------------------------------------
 *  3. Receiver ID  (1 to 255)
 *     Only matters if you add a second receiver later - give each its own.
 * ------------------------------------------------------------------------- */
#define RECEIVER_ID             1

/* ---------------------------------------------------------------------------
 *  4. Which transmitters to listen to
 *     Transmitters use MAC addresses 1a:00:00:00:00:<NODE_ID>.
 *     Packets from any other device are ignored.
 * ------------------------------------------------------------------------- */
#define NODE_MAC_PREFIX         { 0x1a, 0x00, 0x00, 0x00, 0x00 }

/* ---------------------------------------------------------------------------
 *  5. Batching (how CSI samples are packed into UDP packets to the Pi)
 *     A batch is sent when it's full or BATCH_FLUSH_MS old, whichever first.
 *     1400 bytes fits in one WiFi frame.
 * ------------------------------------------------------------------------- */
#define MAX_UDP_PAYLOAD         1400        /* Bytes per UDP packet */
#define BATCH_FLUSH_MS          50          /* Max time a sample waits */
#define CSI_QUEUE_LENGTH        64          /* Samples buffered between tasks */

/* ---------------------------------------------------------------------------
 *  6. Status LED
 *     -2 = use the board's built-in LED (LED_BUILTIN). On the ESP32-S3
 *          DevKitC that's the RGB LED, which lights white.
 *     Any other number = that GPIO. -1 = no LED.
 *     Meaning:  off = no WiFi | solid = WiFi OK, no CSI yet | blinking = working
 *     Seeed XIAO ESP32S3: use 21 and STATUS_LED_ACTIVE_LOW 1.
 * ------------------------------------------------------------------------- */
#define STATUS_LED_GPIO         -2
#define STATUS_LED_ACTIVE_LOW   0

/* ---------------------------------------------------------------------------
 *  7. Serial Monitor (set it to 115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000

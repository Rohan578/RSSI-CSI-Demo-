/*
 * ============================================================================
 *  config.h  -  SETTINGS for the ESP32-S3 CSI receiver
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. WiFi network
 *     2.4 GHz, WPA2. The transmitters copy this network's channel at
 *     power-up (their AUTO_CHANNEL_SSID must be the same name).
 *     WIFI_PASSWORD stays a placeholder in git: run
 *     deploy/fill_wifi_password.sh on the laptop just before uploading.
 *
 *     Current setup: the home network ORBI21 (it has a router + DHCP).
 *     For the isolated linksys222 network instead, see the values in the
 *     comments of sections 2 and 2b.
 * ------------------------------------------------------------------------- */
#define WIFI_SSID               "ORBI21"
#define WIFI_PASSWORD           "__WIFI_PASSWORD__"

/* ---------------------------------------------------------------------------
 *  2. Raspberry Pi (where the CSI data goes)
 *     PI_SERVER_HOST: the Pi's name, looked up with mDNS (".local") at
 *       start-up and after every WiFi reconnect - so it keeps working when
 *       the router gives the Pi a different address. "" = don't look up.
 *     PI_SERVER_IP: used if the name can't be found.
 *       ORBI21: the Pi's current address (172.16.0.75 on 2026-11-06).
 *       linksys222: "192.168.1.50" (fixed by deploy/setup_pi.sh).
 * ------------------------------------------------------------------------- */
#define PI_SERVER_HOST          "csi-pi"
#define PI_SERVER_IP            "172.16.0.75"
#define PI_SERVER_PORT          5005        /* Must match UDP_PORT in server.py */

/* ---------------------------------------------------------------------------
 *  2b. This receiver's own IP address
 *     "" = get one from the network's DHCP server (ORBI21's router).
 *     linksys222 has no router: use a fixed address there instead -
 *       STATIC_IP "192.168.1.60", STATIC_GATEWAY "192.168.1.50".
 * ------------------------------------------------------------------------- */
#define STATIC_IP               ""
#define STATIC_GATEWAY          "192.168.1.50"   /* only used with STATIC_IP */
#define STATIC_SUBNET           "255.255.255.0"

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

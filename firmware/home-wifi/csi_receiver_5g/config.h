/*
 * ============================================================================
 *  config.h  -  SETTINGS for the 5 GHz CSI receiver (home WiFi version)
 *               Board: ESP32-C5 DevKit (4 MB flash), "ESP32C5 Dev Module"
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. Band
 *     1 = 5 GHz (this setup). The C5 measures CSI on the home router's
 *         replies to its pings: the link "router -> C5" on 5 GHz.
 *     0 = 2.4 GHz: the C5 then works as another 2.4 GHz receiver (hears the
 *         transmitters, sends channel beacons). Give it RECEIVER_ID 3 then.
 *     One radio = one band at a time.
 * ------------------------------------------------------------------------- */
#define USE_5GHZ                1

/* ---------------------------------------------------------------------------
 *  2. Home WiFi
 *     Same network as the 2.4 GHz receiver. If your router gives its 5 GHz
 *     band its own name (e.g. "ORBI21-5G"), put that name here.
 *     WPA2 or WPA2/WPA3 mixed. Not a guest network.
 *     5 GHz: fix the router's 5 GHz channel to 36, 40, 44 or 48 if you can.
 *     Channels 52-144 are "DFS": the router may jump off them for radar.
 * ------------------------------------------------------------------------- */
#define WIFI_SSID               "ORBI21"
#define WIFI_PASSWORD           "__WIFI_PASSWORD__"

/* ---------------------------------------------------------------------------
 *  2b. Pin ONE access point (optional, for mesh WiFi)
 *     The 5 GHz BSSID of the box you want (printed at start-up). "" = any.
 * ------------------------------------------------------------------------- */
#define WIFI_BSSID              ""

/* ---------------------------------------------------------------------------
 *  3. This receiver's own IP address  ("" = DHCP)
 *     ORBI21 has two DHCP servers, so use a fixed address on the Orbi's
 *     network (different from the S3's 172.16.0.240).
 * ------------------------------------------------------------------------- */
#define STATIC_IP               "172.16.0.241"
#define STATIC_GATEWAY          "172.16.0.1"
#define STATIC_SUBNET           "255.255.255.0"

/* ---------------------------------------------------------------------------
 *  4. Where the CSI goes: the laptop running server/server.py
 *     Found automatically from the server's announcements (UDP 5006).
 *     SERVER_IP / SERVER_HOST only as a start-up hint or if announcements
 *     are blocked. "" = wait for the announcement.
 * ------------------------------------------------------------------------- */
#define SERVER_IP               ""
#define SERVER_HOST             ""
#define SERVER_PORT             5005
#define DISCOVERY_PORT          5006

/* ---------------------------------------------------------------------------
 *  5. Receiver ID  (must match "receivers" in server/room.json)
 *     2 = the 5 GHz C5.
 * ------------------------------------------------------------------------- */
#define RECEIVER_ID             2

/* ---------------------------------------------------------------------------
 *  6. Where the CSI comes from  (1 = on, 0 = off)
 *  CSI_FROM_ACCESS_POINT: ping the router PING_RATE_HZ times a second and
 *     measure CSI on every reply. Shows up on the laptop as "node 0".
 *     This is the 5 GHz data source (the transmitters are 2.4 GHz only).
 *  CSI_FROM_TRANSMITTERS: also measure the ESP-NOW transmitters - only
 *     useful on 2.4 GHz (USE_5GHZ 0).
 * ------------------------------------------------------------------------- */
#define CSI_FROM_ACCESS_POINT   1
#define CSI_FROM_TRANSMITTERS   1

#define PING_RATE_HZ            50          /* 50-100; more = finer detail on this link */
#define PING_TARGET_IP          ""          /* "" = the gateway (the router) */

/* ---------------------------------------------------------------------------
 *  7. Channel beacons (only on 2.4 GHz, so transmitters can follow)
 * ------------------------------------------------------------------------- */
#define BEACON_RATE_HZ          5

/* ---------------------------------------------------------------------------
 *  8. Transmitter MAC prefix (must match the transmitter sketch)
 * ------------------------------------------------------------------------- */
#define NODE_MAC_PREFIX         { 0x1a, 0x00, 0x00, 0x00, 0x00 }

/* ---------------------------------------------------------------------------
 *  9. Batching
 * ------------------------------------------------------------------------- */
#define MAX_UDP_PAYLOAD         1400
#define BATCH_FLUSH_MS          50
#define CSI_QUEUE_LENGTH        96

/* ---------------------------------------------------------------------------
 *  10. Status LED
 *     -2 = the board's built-in LED (C5 DevKit: RGB LED on GPIO 27, white).
 *     off = no WiFi | fast blink = no server yet | solid = no CSI yet
 *     slow blink = working
 * ------------------------------------------------------------------------- */
#define STATUS_LED_GPIO         -2
#define STATUS_LED_ACTIVE_LOW   0

/* ---------------------------------------------------------------------------
 *  11. Serial Monitor (115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000

/*
 * ============================================================================
 *  config.h  -  SETTINGS for the 2.4 GHz CSI receiver (home WiFi version)
 *               Boards: ESP32-S3 (Seeed XIAO ESP32S3 or DevKit), or
 *               ESP32-C6 as an optional SECOND 2.4 GHz receiver
 * ============================================================================
 *  This is the only file you normally edit. After changing it, click Upload.
 * ============================================================================
 */
#pragma once

/* ---------------------------------------------------------------------------
 *  1. Home WiFi
 *     The receiver joins it to send CSI to the laptop. Must have a 2.4 GHz
 *     band (WPA2 or WPA2/WPA3 mixed). Not a guest network: guest networks
 *     usually block devices from talking to each other.
 *     WIFI_PASSWORD stays a placeholder in git: run
 *     tools/fill_wifi_password.sh just before uploading (or type it in, and
 *     put the placeholder back before committing).
 * ------------------------------------------------------------------------- */
#define WIFI_SSID               "ORBI21"
#define WIFI_PASSWORD           "__WIFI_PASSWORD__"

/* ---------------------------------------------------------------------------
 *  1b. Pin ONE access point (optional, recommended on mesh WiFi like Orbi)
 *     A mesh has several boxes with the same name, possibly on different
 *     2.4 GHz channels. Every 2.4 GHz receiver must be on the SAME channel,
 *     so pin them all to the same box. The receiver prints the one it joined
 *     ("[WIFI] Access point: ..."). "" = any. Falls back to any after 20 s.
 *     0E:02:8E:9B:DA:18 = ORBI21's main router, 2.4 GHz radio (2026-11-06).
 * ------------------------------------------------------------------------- */
#define WIFI_BSSID              ""

/* ---------------------------------------------------------------------------
 *  2. This receiver's own IP address
 *     ""  = DHCP (normal home networks).
 *     ORBI21 has TWO DHCP servers (found 2026-11-06); the second one hands
 *     out 172.27.35.x behind a NAT that loses ~90% of packets. So on ORBI21
 *     use a fixed address on the Orbi's own network, as below. Give the
 *     2nd 2.4 GHz receiver (if any) a different one, e.g. 172.16.0.242.
 * ------------------------------------------------------------------------- */
#define STATIC_IP               "172.16.0.240"
#define STATIC_GATEWAY          "172.16.0.1"     /* the Orbi router */
#define STATIC_SUBNET           "255.255.255.0"

/* ---------------------------------------------------------------------------
 *  3. Where the CSI goes: the laptop running server/server.py
 *     The server announces itself on the WiFi once a second (UDP port
 *     DISCOVERY_PORT), so normally you set NOTHING here: the receiver
 *     finds the laptop by itself, and follows it if its address changes.
 *     SERVER_IP: only used until the laptop is found, or if your router
 *       blocks the announcements. "" = wait for the announcement.
 *     SERVER_HOST: optional mDNS name (e.g. "my-laptop" for my-laptop.local).
 * ------------------------------------------------------------------------- */
#define SERVER_IP               ""
#define SERVER_HOST             ""
#define SERVER_PORT             5005        /* Must match UDP_PORT in server.py */
#define DISCOVERY_PORT          5006        /* Must match DISCOVERY_PORT in server.py */

/* ---------------------------------------------------------------------------
 *  4. Receiver ID  (must match "receivers" in server/room.json)
 *     1 = the main 2.4 GHz receiver (S3). 3 = an optional 2nd 2.4 GHz one.
 *     (2 is the 5 GHz C5.)
 * ------------------------------------------------------------------------- */
#define RECEIVER_ID             1

/* ---------------------------------------------------------------------------
 *  5. Channel beacons for the transmitters
 *     The receiver broadcasts "I'm on channel N" this many times a second.
 *     Transmitters lock onto it and follow it if the router changes channel.
 *     0 = off (then transmitters only copy the router's channel at power-up).
 * ------------------------------------------------------------------------- */
#define BEACON_RATE_HZ          5

/* ---------------------------------------------------------------------------
 *  6. How much CSI to send per sample
 *     The server only uses the first 128 bytes (the "LLTF" block: 52
 *     subcarriers). Sending just those cuts the WiFi traffic to the laptop
 *     to about a third - fewer lost packets on a busy home network.
 *     128 = LLTF only (S3 / ESP32 / C3).  0 = everything the chip gives.
 *     The C6 lays out its CSI differently, so it always sends everything.
 * ------------------------------------------------------------------------- */
#define CSI_SEND_BYTES          128

/* ---------------------------------------------------------------------------
 *  7. Which transmitters to listen to (MAC 1a:00:00:00:00:<NODE_ID>)
 * ------------------------------------------------------------------------- */
#define NODE_MAC_PREFIX         { 0x1a, 0x00, 0x00, 0x00, 0x00 }

/* ---------------------------------------------------------------------------
 *  8. Batching (CSI samples per UDP packet)
 *     Sent when full or BATCH_FLUSH_MS old. 1400 bytes fits one WiFi frame.
 *     Queue: 10 transmitters x 50/s = 500 samples/s; 128 gives ~250 ms slack.
 * ------------------------------------------------------------------------- */
#define MAX_UDP_PAYLOAD         1400
#define BATCH_FLUSH_MS          50
#define CSI_QUEUE_LENGTH        128

/* ---------------------------------------------------------------------------
 *  9. Status LED
 *     off = no WiFi | solid = WiFi OK, no CSI yet | blinking = working
 *     fast blink = WiFi OK but the laptop server hasn't been found yet
 * ------------------------------------------------------------------------- */
#if defined(ARDUINO_XIAO_ESP32S3)          /* Tools > Board: XIAO_ESP32S3 */
  #define STATUS_LED_GPIO       21         /* small yellow LED, lights when LOW */
  #define STATUS_LED_ACTIVE_LOW 1
#elif CONFIG_IDF_TARGET_ESP32C6            /* C6 SuperMini */
  #define STATUS_LED_GPIO       15
  #define STATUS_LED_ACTIVE_LOW 0
#else
  #define STATUS_LED_GPIO       -2         /* -2 = the board's LED_BUILTIN */
  #define STATUS_LED_ACTIVE_LOW 0
#endif

/* ---------------------------------------------------------------------------
 *  10. Serial Monitor (115200 baud)
 * ------------------------------------------------------------------------- */
#define SERIAL_BAUD             115200
#define STATS_PRINT_INTERVAL_MS 5000

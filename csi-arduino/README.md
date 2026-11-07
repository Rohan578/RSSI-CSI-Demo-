# WiFi CSI Sensing — Arduino IDE version

> **This setup (Nov 2026): linksys222, 5 × ESP32 + 1 × ESP32-S3, 2.4 GHz, basement.**
> See **[README_PI.md](README_PI.md)** for the Pi, the 3D room view, and the bring-up order.
> See **[VOLUNTEERS.md](VOLUNTEERS.md)** for the volunteer briefing and data plan.
>
> Changes from the zip:
>
> - **S3:** gets a fixed IP (`192.168.1.60`), since linksys222 has no DHCP server. It
>   sends to the Pi at `192.168.1.50`.
> - **Transmitters:** find linksys222's channel by themselves at power-up
>   (`AUTO_CHANNEL_SSID`). `WIFI_CHANNEL 11` is only the fallback.
> - **Receiver password:** `WIFI_PASSWORD` is a placeholder in the receiver configs. Run
>   `./deploy/fill_wifi_password.sh` before uploading.
> - **Pi server:** extended with a 3D view, guided volunteer collection, calibration,
>   zone training and a simulator.

Camera-free posture, activity and fall detection using WiFi Channel State Information (CSI).

```
 2.4 GHz                                         5 GHz (optional)
 Transmitters (ESP32 / C6 / C3)                  Access point
   ESP-NOW, 50 packets/s each                      answers the C5's pings, 50/s
        │                                               │
        ▼                                               ▼
 ESP32-S3 receiver 1 (dipole antenna)            ESP32-C5 receiver 2
        │  UDP over the access point's WiFi             │  UDP
        └───────────────────┬───────────────────────────┘
                            ▼
                     Raspberry Pi 5
   logs labeled data (both bands) to CSV, live dashboard, (later) the AI model
```

Each sender → receiver path is a **link**. People in the room change how the signal travels along each link; the AI learns those patterns. On the Pi, **node 0 = the access point**.

Based on Espressif's official examples `csi_send` and `csi_recv` from [github.com/espressif/esp-csi](https://github.com/espressif/esp-csi).

## What's in this folder

```
csi_transmitter/           ONE sketch for ESP32, ESP32-C6 SuperMini, ESP32-C3 and ESP32-C5
  csi_transmitter.ino        the code (9 labeled sections)
  config.h                   <- EDIT: NODE_ID, WIFI_CHANNEL (and USE_5GHZ on a C5)
  espnow_packet.h            what each packet contains
csi_receiver_s3/           the ESP32-S3 receiver
  csi_receiver_s3.ino        the code (11 labeled sections)
  config.h                   <- EDIT: WiFi name/password, Pi's IP address
  csi_packet.h               UDP format sent to the Pi (must match server.py)
  espnow_packet.h            identical copy of the transmitter's file
csi_receiver_c5/           the ESP32-C5 dual-band receiver (optional, 2.4 or 5 GHz)
  csi_receiver_c5.ino        the code (12 labeled sections)
  config.h                   <- EDIT: USE_5GHZ, WiFi name/password, Pi's IP
  csi_packet.h, espnow_packet.h   identical copies of the S3's files
pi_server/                 runs on the Raspberry Pi
  server.py                  receives, logs CSV, dashboard on port 5000
  fake_receiver.py           sends fake CSI - test without any boards
  compare_bands.py           compares 2.4 vs 5 GHz in a recording, flags interference
SETUP_DUAL_BAND.md         fill-in setup sheet for the S3 + C5 dual-band setup
  requirements.txt
```

---

## Arduino IDE setup (once)

1. **File > Preferences > Additional boards manager URLs**, add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools > Board > Boards Manager**, search "esp32", install **"esp32 by Espressif Systems" version 3.3.0 or newer.**
3. No extra libraries are needed.

| Board | Tools > Board | Tools > USB CDC On Boot |
|---|---|---|
| ESP32 DevKit / WROOM-32 | ESP32 Dev Module | (not shown) |
| ESP32-C6 SuperMini | ESP32C6 Dev Module | **Enabled** |
| ESP32-C3 SuperMini | ESP32C3 Dev Module | **Enabled** |
| ESP32-C5 | ESP32C5 Dev Module (or your exact board) | **Enabled** if plugged into the native "USB" port, **Disabled** if plugged into the "UART"/"COM" port |
| ESP32-S3 | ESP32S3 Dev Module (or your exact board) | **Enabled** if plugged into the native "USB" port, **Disabled** if plugged into the "UART"/"COM" port |

Serial Monitor: **115200 baud** for all boards.

If an upload won't start, hold the **BOOT** button while plugging in USB (or while "Connecting..." shows), then release.

---

## Setup, in this order

### 1. Access point
- 2.4 GHz network, **WPA2**, **20 MHz** channel width
- **Fixed channel** (1, 6 or 11 — pick the quietest using a WiFi analyzer app). Never "auto".
- Turn off band steering / "smart connect" for this network.
- Give the Pi a **DHCP reservation** (fixed IP) in the access point's settings.

### 2. Raspberry Pi
Connect it to the access point (Ethernet is best), then:

```bash
hostname -I                     # note the IP, e.g. 192.168.1.50
cd pi_server
pip install -r requirements.txt     # or: sudo apt install python3-flask
python3 server.py
```

Dashboard: `http://<pi-ip>:5000` from any device on the network.

### 3. ESP32-S3 receiver
1. Open `csi_receiver_s3/csi_receiver_s3.ino`.
2. In the **config.h** tab: `WIFI_SSID`, `WIFI_PASSWORD`, `PI_SERVER_IP` (from step 2).
3. Upload, open the Serial Monitor. You'll see:

```
[WIFI] Connected. S3 IP: 192.168.1.73
--------------------------------------------------
  ACCESS POINT CHANNEL: 6
  Set WIFI_CHANNEL to this number on EVERY transmitter.
--------------------------------------------------
[CSI] Capturing CSI from transmitters 1a:00:00:00:00:xx
```

**Write down that channel number.**

### 3b. ESP32-C5 receiver (optional: adds 5 GHz)
1. On the access point, fix the **5 GHz** network to channel **36, 40, 44 or 48** (or 149–165), **20 MHz** wide. Avoid channels 52–144 (DFS): the access point can jump off them without warning.
2. Open `csi_receiver_c5/csi_receiver_c5.ino`. In **config.h**: `USE_5GHZ 1`, `WIFI_SSID` (the 5 GHz network's name if it has its own), `WIFI_PASSWORD`, `PI_SERVER_IP`. Leave `RECEIVER_ID 2`.
3. Upload. The Serial Monitor shows:

```
  BAND: 5 GHz    ACCESS POINT CHANNEL: 36
[CSI] Sources: access point (node 0)  transmitters 1a:00:00:00:00:xx
[PING] Pinging 192.168.1.1 50 times per second
...
  access pt | CSI/s  50.0 | pings ok/s  50.0 | RSSI  -55 dBm | CSI 256 bytes | ping timeouts 0
  No transmitters heard on channel 36 (normal on 5 GHz unless you
  have a C5 transmitter on this channel).
```

The C5 pings the access point and measures CSI on every reply. That's the access point → C5 link, shown on the dashboard as **"Access point → receiver 2 (5 GHz)"**. It needs no extra transmitter, which matters because the ESP32/C6/C3 transmitters can't send on 5 GHz.

**Where to put the C5:** across the room from the access point, so the access point → C5 link crosses the area people move through, just like the transmitter links. Don't move the access point once you start collecting data. It's a sender now.

### 4. Transmitters (repeat for each board)
1. Open `csi_transmitter/csi_transmitter.ino`.
2. In **config.h**: set `NODE_ID` (**different on every board**) and `WIFI_CHANNEL` (from step 3).
   Suggested IDs: ESP32 = 1–19, C6 = 20–39, C3 = 40–59.
3. Pick the right board in Tools (table above) and upload. The LED pin is chosen automatically for the board you pick.
4. Serial Monitor every 5 s:

```
node 20 | pkt/s 50.0 | sent 250 | air ok 250 | air fail 0 | busy 0 | errors 0
```

5. **Label the board with its node ID** and note where it sits in the room.

### 5. Check it all works
The S3's Serial Monitor prints one line per transmitter every 5 s:

```
[STATS] WiFi OK | CSI total 1503 | sent to Pi 1503 samples in 501 UDP packets | queue full 0 | UDP errors 0
  node   1 | CSI/s  49.8 | heard/s  49.8 | RSSI  -52 dBm | CSI 384 bytes
  node  20 | CSI/s  50.0 | heard/s  50.0 | RSSI  -47 dBm | CSI 384 bytes
```

The dashboard then shows one card per link ("Node 20 → receiver 1 · 2.4 GHz"), each with its rate, signal strength, lost packets, motion score and live CSI shape. With the C5 running, a row at the top compares the bands: links, samples/s, average signal strength and average motion score for **2.4 GHz vs 5 GHz**, live.

---

## Collecting training data
1. Type a label on the dashboard (`empty`, `standing`, `sitting`, `lying`, `walking`, `falling`) and click **Set label**.
2. Act it out for a few minutes. Do falls onto a **gym mat or mattress**.
3. Change the label and repeat. Record an `empty` room every session.

Each run creates `pi_server/data/csi_<date>_<time>.csv`. Every row is one CSI sample with its label, `receiver_id`, `tx_node_id` (0 = access point), `band`, `tx_seq` (packet counter) and the raw CSI values (`imag real imag real ...`). Group rows by `(receiver_id, tx_node_id, band)` to get one stream per link.

**To compare bands after a session:** run `python3 compare_bands.py`. It lines up both receivers by time, compares them second by second, and flags windows where only 2.4 GHz saw movement (likely interference) as `use_for_training = 0`. The dashboard shows the same check live.

**To measure what 5 GHz adds:** train one model on only the 2.4 GHz rows and another on 2.4 GHz + 5 GHz rows, and test both on the same held-out session. The difference in accuracy shows what 5 GHz adds.

**No hardware yet?** Run `python3 fake_receiver.py` next to `server.py`. It sends fake CSI from nodes 1, 2 and 3 that switch between "still" and "moving" every 8 seconds. To fake both receivers at once:

```bash
python3 fake_receiver.py --receiver-id 1 --channel 6  --nodes 1 2 20 &
python3 fake_receiver.py --receiver-id 2 --channel 36 --nodes 0
```

---

## Troubleshooting

| What you see | Fix |
|---|---|
| S3: "No transmitters heard yet" | Transmitter `WIFI_CHANNEL` doesn't match the channel the S3 printed. Or the transmitter isn't powered — check its Serial Monitor. |
| S3: node has `heard/s` but `CSI/s` is 0 | Shouldn't happen with these settings — check you're on Arduino core 3.3+. |
| S3: everything stops after the access point restarts | The access point picked a new channel. Set it to a fixed channel. |
| S3: `UDP errors` climbing | `PI_SERVER_IP` is wrong, or the Pi isn't on the same network. |
| Dashboard empty but S3 says "sent to Pi" | Pi firewall: `sudo ufw allow 5005/udp` (if ufw is on). Is `server.py` running? |
| Transmitter: `busy` climbing | Channel is congested. Pick a quieter access point channel (and update every transmitter), or lower `SEND_RATE_HZ`. |
| Lots of lost samples on one link | That transmitter is too far or blocked. Move it closer or check its antenna. |
| Serial Monitor blank (C6/C3/S3) | Wrong "USB CDC On Boot" setting — see the table above. |
| Upload fails | Hold BOOT while connecting. Close the Serial Monitor first. |
| C5 never connects on 5 GHz | The access point's 5 GHz network may have a different name (e.g. "MyWiFi-5G"), be turned off, or be WPA3-only. Use WPA2. |
| C5: `pings ok/s` is 0 | The gateway isn't answering pings. Set `PING_TARGET_IP` to the Pi's IP instead. |
| C5: pings OK but access point `CSI/s` is 0 | Check you're on Arduino core 3.3+ and `CSI_FROM_ACCESS_POINT` is 1. |
| C5 5 GHz data stops randomly | The access point is on a DFS channel (52–144) and jumped away. Use 36–48. |
| Transmitter: `#error ... no 5 GHz radio` | `USE_5GHZ` is 1 on a chip that isn't a C5. Only the C5 can use 5 GHz. |

## Rules for good data
- **Same `WIFI_CHANNEL`, `SEND_RATE_HZ` and `TX_POWER_DBM` on every transmitter.**
- **Never move, swap or re-number a node** once you start collecting. Keep a list: node ID → position.
- **S3 dipole antenna: mount it vertically and fix it in place.** A moving antenna or cable changes the CSI. Some S3 boards need a resistor/jumper moved to use the external antenna connector — check your board's documentation.
- Place the S3 on one side of the room and spread transmitters along the opposite walls, so the links cross the area people move through.

## Notes
- The earlier **ESP-IDF transmitter projects send the exact same packets**, so ESP-IDF and Arduino transmitters can be mixed freely.
- The S3 receiver sketch can also be built for an **ESP32-C6** (pick "ESP32C6 Dev Module") if you want to compare it against the S3. Espressif rates the C6 higher for CSI quality.
- **One C5 = one band at a time.** It has a single radio. To record both bands at the same moment, run the S3 on 2.4 GHz and the C5 on 5 GHz together.
- **More 5 GHz links:** extra C5 boards running `csi_transmitter` with `USE_5GHZ 1` and the access point's 5 GHz channel are heard by the C5 receiver, just like transmitters on 2.4 GHz.
- The C5 sketches use about 1 MB of the C5's 4 MB flash. CSI is streamed to the Pi, never stored on the board, so flash size doesn't limit anything.

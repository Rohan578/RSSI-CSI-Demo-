# Flashing the ESP32 boards

This guide is for the current setup: **1 × ESP32-S3 receiver + 5 × ESP32 transmitters**, on
the home WiFi **ORBI21**, sending to the Raspberry Pi at **`csi-pi.local`**. Follow it top to
bottom on any computer with the Arduino IDE. It takes about 20 minutes.

> **Only flash the sketches inside `csi-arduino/`:** `csi_receiver_s3/` and
> `csi_transmitter/`. The old files in `legacy/` don't work with the Pi.

---

## What you need

- **Arduino IDE 2.x**
- In **Boards Manager**, **"esp32 by Espressif Systems" version 3.3.0 or newer**. If it
  isn't listed:
  1. Go to File → Preferences → Additional boards manager URLs.
  2. Add `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.

  No other libraries are needed. mDNS, used to find the Pi, is part of the ESP32 core.
- USB cables that carry **data**, not just charging.
- **The ORBI21 WiFi password.** Only the S3 needs it.
- The Pi powered on. Check: http://csi-pi.local:5000 opens in a browser on ORBI21.

## 1. Get the code from `main`

Pick one:

```bash
git clone https://github.com/Rohan578/RSSI-CSI-Demo-.git     # first time
cd RSSI-CSI-Demo-
git checkout main && git pull                                # every time after that
```

Or on GitHub: **Code → Download ZIP**, then unzip it.

## 2. Put the WiFi password into the S3 sketch (S3 only)

The password is **never stored in git**. `config.h` holds the placeholder
`__WIFI_PASSWORD__`. Fill it in locally just before uploading:

- **On the Linux laptop that has ORBI21 saved,** run
  `./csi-arduino/deploy/fill_wifi_password.sh`. It copies the saved ORBI21 password into
  `config.h` and never prints it.
- **On any other computer,** open `csi-arduino/csi_receiver_s3/config.h`, find the line
  ```c
  #define WIFI_PASSWORD           "__WIFI_PASSWORD__"
  ```
  and replace `__WIFI_PASSWORD__` with the ORBI21 password. Keep the quotes.

Leave the rest of `config.h` as it is. These values already match the Pi:

| Setting | Value | Meaning |
|---|---|---|
| `WIFI_SSID` | `"ORBI21"` | The network the S3 joins |
| `PI_SERVER_HOST` | `"csi-pi"` | The S3 finds the Pi as `csi-pi.local`, even if its address changes |
| `PI_SERVER_IP` | `"172.16.0.75"` | Used only if `csi-pi.local` doesn't answer |
| `PI_SERVER_PORT` | `5005` | The Pi listens on **UDP 5005** |
| `STATIC_IP` | `"172.16.0.240"` | A **fixed** address on the Orbi's network (see below) |
| `STATIC_GATEWAY` | `"172.16.0.1"` | The Orbi router |
| `WIFI_BSSID` | `""` | Any ORBI21 access point. Pinning one is only needed in special cases |

**Why a fixed address:** ORBI21 has **two DHCP servers**:
- the Orbi (172.16.0.x);
- another device handing out 172.27.35.x behind its own NAT. Through it, about 90% of
  the packets never reach the Pi.

With DHCP, the S3 sometimes gets the bad one. 172.16.0.240 was free on 2026-11-06; reserve
it for the S3 in the Orbi app if you can.

**Don't commit `config.h` with the password in it.** Step 6 puts the placeholder back.

## 3. Flash the ESP32-S3 receiver

1. In the Arduino IDE, choose **File → Open** →
   `csi-arduino/csi_receiver_s3/csi_receiver_s3.ino`. The tabs `config.h`, `csi_packet.h` and
   `espnow_packet.h` open with it.
2. Set **Tools**:

   | Your S3 board | Tools → Board | Other Tools settings |
   |---|---|---|
   | **Seeed Studio XIAO ESP32S3** (this setup) | **XIAO_ESP32S3** | Leave the defaults. Then the status LED (GPIO 21) works |
   | Espressif ESP32-S3 DevKitC | ESP32S3 Dev Module | USB CDC On Boot: **Enabled** on the native **USB** port, **Disabled** on the **UART/COM** port |

   Then pick the S3's **Port** (on Linux usually `/dev/ttyACM0`).

   **XIAO: screw or click the antenna onto its tiny U.FL connector first.** The XIAO has no
   built-in antenna, and without one its WiFi barely works.

3. Click **Upload**. If it hangs at "Connecting...", hold the **BOOT** button until the upload
   starts.
4. Open **Tools → Serial Monitor**, set **115200 baud**, and press **RESET** on the board.
   **This is what success looks like:**

   ```
   ==================================================
     CSI receiver 1  |  sending to csi-pi.local (else 172.16.0.75):5005
   ==================================================
   [WIFI] Using fixed IP 172.16.0.240
   [WIFI] Connecting to "ORBI21"....
   [WIFI] Connected. S3 IP: 172.16.0.240
   [WIFI] Access point: 0E:02:8E:9C:00:06 (put this in WIFI_BSSID to always use it)
   [WIFI] Signal from access point: -47 dBm
   --------------------------------------------------
     ACCESS POINT CHANNEL: 1
     ...
   --------------------------------------------------
   [PI] Found csi-pi.local at 172.16.0.75
   [CSI] Capturing CSI from transmitters 1a:00:00:00:00:xx

   [STATS] WiFi OK | CSI total 0 | sent to Pi 0 samples ...
     No transmitters heard yet. Check they're powered and on the
     same channel as the access point (channel 1).
   ```

   **Write down the `ACCESS POINT CHANNEL` number.** "No transmitters heard yet" is normal
   until step 4.

**If you see something else instead:**

| Serial Monitor shows | Meaning and fix |
|---|---|
| `[ERROR] WIFI_PASSWORD in config.h is still the placeholder.` | Step 2 was skipped. Fill in the password, then upload again |
| `Connecting to "linksys222"....` (forever) | You flashed old settings. Get the code from `main` again (step 1) |
| `Still not connected to "ORBI21"` | Wrong password, or the S3 is too far from an ORBI21 access point. Move it closer to test |
| `[PI] csi-pi.local didn't answer - sending to 172.16.0.75` | The Pi is off, or not on ORBI21. Check http://csi-pi.local:5000. It still works if the Pi really is at 172.16.0.75 |
| `S3 IP: 172.27.35.x` | It took an address from the second DHCP server, and packets will be lost. Keep `STATIC_IP "172.16.0.240"` (step 2 table) |
| The stats show `queue full` climbing | Old firmware: flash the current code. It no longer stalls when no Serial Monitor is open |
| Nothing at all | Wrong **USB CDC On Boot** setting (see the table above), or the wrong baud rate |

## 4. Flash ONE transmitter first (node 1)

1. **File → Open** → `csi-arduino/csi_transmitter/csi_transmitter.ino`.
2. In the `config.h` tab, set **`#define NODE_ID 1`**. The transmitters need **no
   password**: they never join the WiFi, they only copy ORBI21's channel.
3. Set **Tools**:

   | Board in your hand | Tools → Board | USB CDC On Boot |
   |---|---|---|
   | ESP32 DevKit / WROOM-32 | **ESP32 Dev Module** | (not shown) |
   | ESP32-C3 SuperMini | ESP32C3 Dev Module | Enabled |
   | ESP32-C6 SuperMini | ESP32C6 Dev Module | Enabled |

4. **Upload**, then open the Serial Monitor at **115200** and press RESET:

   ```
   [RADIO] Scanning for "ORBI21" to copy its channel (try 1)...
   [RADIO] Found "ORBI21" on channel 1 (-48 dBm)
   [RADIO] 2.4 GHz, channel 1, 20 MHz, TX power 15.00 dBm
   [ESPNOW] Broadcasting at 802.11n MCS0, 20 MHz
   ==================================================
     CSI transmitter
     Node ID  : 1
     MAC      : 1a:00:00:00:00:01
     ...
   ==================================================
   node 1 | pkt/s 50.0 | sent 250 | air ok 250 | air fail 0 | busy 0 | errors 0
   ```

   **The channel must be the same number the S3 printed.**
5. **Check the whole chain:**
   - **S3 Serial Monitor:** a line like `node   1 | CSI/s  49.8 | heard/s  49.8 | RSSI  -50 dBm`.
   - **Pi:** http://csi-pi.local:5000 shows a "Node 1 → receiver 1" card at about 50
     samples/s. http://csi-pi.local:5000/room shows the TX 1 path live.

   **Don't continue until node 1 shows up on the Pi.**

**If you see something else instead:**

| Symptom | Fix |
|---|---|
| Transmitter: `"ORBI21" not found - using WIFI_CHANNEL 1` | It couldn't see ORBI21's 2.4 GHz band. Power-cycle it near an ORBI21 access point. Its channel must match the S3's |
| S3: `No transmitters heard yet` while the transmitter says `pkt/s 50` | Different channels. Compare the S3's `ACCESS POINT CHANNEL` with the transmitter's channel, and power-cycle the transmitter |
| S3 hears node 1, but the Pi shows nothing | See the S3's `[PI]` line. The Pi must be on ORBI21 and listening on UDP 5005 |

## 5. Flash the other four transmitters

Repeat step 4 with **`NODE_ID` 2, 3, 4 and 5**, a different number on every board. **Label
each board with its number** (tape and a marker). The positions in `pi_server/room.json`
refer to these numbers.

Each new node should appear in the S3's Serial Monitor and on the Pi within a few seconds.

## 6. Put the placeholder back

- **Linux laptop:** `./csi-arduino/deploy/fill_wifi_password.sh --clear`
- **Anywhere else:** in `csi_receiver_s3/config.h`, put `__WIFI_PASSWORD__` back in place
  of the password.

**Optional safety net,** once per copy of the repo:

```bash
git config core.hooksPath .githooks
```

After that, git **refuses** any commit that contains a real `WIFI_PASSWORD`.

## 7. After flashing

- **Place the boards** (`VOLUNTEERS.md` → "Setting up the room"). Put them all at the
  same height, about 1 m, and **never move them** once you start recording.
- **Measure where they ended up** and put the positions into `csi-arduino/pi_server/room.json`
  on the Pi. Then run `ssh csi-pi 'sudo systemctl restart csi-server'`.
- **Calibrate:** open http://csi-pi.local:5000/room, have everyone leave the room, and
  press **Calibrate empty room**.
- **If ORBI21 later changes its 2.4 GHz channel,** the transmitters go silent.
  **Power-cycle the transmitters** and they pick up the new channel. To avoid this
  entirely, fix the 2.4 GHz channel in the Orbi's settings.

## Settings that must match

| Where | Setting | Must equal |
|---|---|---|
| S3 `config.h` | `WIFI_SSID` | the network name (`ORBI21`) |
| Transmitter `config.h` | `AUTO_CHANNEL_SSID` | the same name (`ORBI21`) |
| Transmitter `config.h` | `NODE_ID` | 1–5, a different number on every board, matching `room.json` |
| Transmitter `config.h` | `SEND_RATE_HZ`, `TX_POWER_DBM` | the same on every board (50, 15) |
| S3 `config.h` | `PI_SERVER_PORT` | `UDP_PORT` in `pi_server/server.py` (5005) |
| S3 `config.h` | `PI_SERVER_HOST` | the Pi's hostname (`csi-pi`) |
| S3 `config.h` | `RECEIVER_ID` | `room.json` → `receivers` (1) |

**Going back to the isolated linksys222 network:** each `config.h` notes the linksys222
values in its comments. They are `WIFI_SSID` / `AUTO_CHANNEL_SSID` `"linksys222"`,
`STATIC_IP "192.168.1.60"` and `PI_SERVER_IP "192.168.1.50"`. See the top-level README,
"Network design".

# WiFi CSI Room Sensing: home WiFi + dual band (branch `home-wifi-dual-band`)

Cheap ESP32 boards send WiFi packets across a room. A person's body changes how those radio waves travel, and the WiFi chip measures that as **Channel State Information (CSI)**. A laptop collects the CSI, works out roughly where people are, and shows it as a 3D model of the room.

**This branch is the version that runs on your normal home WiFi.** There's no Raspberry Pi and no separate access point: the receivers join your home network and send to a laptop running the server.

![3D room view: the room, the transmitters, the receiver, glowing radio paths, and a person estimated from WiFi alone (simulator, flash-ready layout)](docs/images/room-3d-view.png)

It's built on the `flash-ready` branch, the code that was tested on real hardware, with these upgrades:

- **Up to 10 transmitters**, any mix of ESP32, ESP32-C3, ESP32-C6 and ESP32-C5, all on 2.4 GHz.
- **Two receivers:**
  - an **ESP32-S3 on 2.4 GHz**, which hears the transmitters;
  - an **ESP32-C5 DevKit (4 MB) on 5 GHz**, which measures your home router's signal.
- **More reliable on home WiFi.** Transmitters follow the router's channel by themselves, and receivers find the laptop by themselves.
- **More accurate.** Home-WiFi interference no longer shows up as ghost people. The improvement is measured on the simulator; see [Accuracy](#accuracy).

> Branch history: `main` (first uploads) → `csi-pi-dashboard` / `pi-3d-dashboard` (Pi + 3D view) → `flash-ready` (tested on hardware) → **`home-wifi-dual-band`** (this one).

---

## Contents

1. [Try it now, no hardware](#try-it-now-no-hardware)
2. [How it works](#how-it-works)
3. [What changed from flash-ready, and why](#what-changed-from-flash-ready-and-why)
4. [Hardware](#hardware)
5. [Channels and settings cheat sheet](#channels-and-settings-cheat-sheet)
6. [Setup, step by step](#setup-step-by-step)
7. [Placing the boards](#placing-the-boards)
8. [What each Serial Monitor should say](#what-each-serial-monitor-should-say)
9. [Accuracy](#accuracy)
10. [Collecting data and training the AI model](#collecting-data-and-training-the-ai-model)
11. [Troubleshooting](#troubleshooting)
12. [Repository layout](#repository-layout)
13. [Security notes](#security-notes)
14. [Credits](#credits)

---

## Try it now, no hardware

```bash
cd server
pip install -r requirements.txt          # flask, numpy, scikit-learn
python run_demo.py                       # server + simulated room (10 TX, S3, C5)
python run_demo.py --interference        # ...with home-WiFi interference bursts
```

Open **http://localhost:5000/room**. It works the same on Windows, macOS and Linux.

The simulated room starts empty for 25 s, which is used as the calibration. Then 1, 2 and 3 people walk in and out, in a loop. The white outlines show where the simulated people really are, so you can see the error.

---

## How it works

```
                         THE ROOM (default: 20 x 20 ft basement)

   TX 3 . TX 4 ...... TX 5 ...... TX 6 ...... TX 7 . TX 8        10 transmitters (2.4 GHz)
    |  \     \          |          /          /     /  |          ESP-NOW broadcast,
   TX 2   \    \        |        /         /     /   TX 9         50 packets/s each,
    |  \    \    \      |      /        /     /    /   |          MAC 1a:00:00:00:00:<id>
   TX 1  \    \    \    |    /       /     /    /    TX 10
       \   \    \   \   |   /     /    /    /    /
         [ C5 ]       [ S3 receiver ]                   S3: 2.4 GHz, hears all 10 paths
           ^             |                              C5: 5 GHz, pings the router
           |             | joined to home WiFi (2.4)
   home router  <--------+-------------------- UDP CSI --------------->  LAPTOP
   (5 GHz link to the C5 = "node 0")                                    server/server.py
                                                                        http://<laptop>:5000/room
```

**The data path:**

1. **Transmitters** broadcast a 12-byte ESP-NOW packet 50 times a second. They never join the WiFi, so they need no password.
2. **The S3 receiver** is joined to your home WiFi on 2.4 GHz.
   - It listens in promiscuous mode and measures CSI on every transmitter packet: 52 subcarriers, each telling how much that frequency was weakened.
   - Five times a second it broadcasts a tiny **beacon**: "I'm on channel N". Transmitters lock onto that channel, and if the router moves to another channel they follow.
3. **The C5 receiver** is joined to the same WiFi on **5 GHz**. It pings the router 50 times a second and measures CSI on every reply: the **router → C5** path on 5 GHz.
   - 5 GHz has half the wavelength (6 cm vs 12.5 cm), so it reacts to smaller movements.
   - 5 GHz is free of most home interference: no microwaves, no Bluetooth.
   - That gives the server a second, independent view.
4. **Both receivers send their CSI over UDP to the laptop.** They find it on their own: `server.py` announces itself on the WiFi once a second (UDP port 5006). The laptop can get any IP address, and you never type one into a sketch.
5. **The server** works on the last second of data from every path, four times a second:
   - It compares each path with the empty room (**calibration**).
   - It removes interference that hits all paths at once (**common-mode removal**).
   - It finds the floor spot that best explains which paths changed (**geometry**), or uses the **AI model** once you've trained one.
   - It tracks people, and shows them in 3D at `/room`.

**Why "common mode" matters at home:** other devices' 2.4 GHz traffic disturbs every path of a receiver at the same moment. A person disturbs only the few paths that pass near them. So per receiver, the server subtracts the disturbance that all paths share. While that shared part is high, no *new* person is shown, unless the 5 GHz link (which that interference barely touches) also sees movement.

---

## What changed from flash-ready, and why

### Reliability

| Problem on `flash-ready` | Fix in this branch | Where |
|---|---|---|
| Transmitters copied the router's channel once at power-up. The Orbi changes its 2.4 GHz channel by itself, and then **all transmitters went silent** until power-cycled | Receivers broadcast **channel beacons**. Transmitters lock onto the channel where they hear one, follow it if the receiver moves, and **search again by themselves** after 10 s of silence. Fallback: copy the router's channel by SSID, then `WIFI_CHANNEL` | `csi_transmitter`, both receivers |
| Up to 10 transmitters on separate clocks can drift into the same time slot and keep colliding | **Random ±2 ms jitter** per packet, plus a start offset per `NODE_ID` | `csi_transmitter` |
| The S3 sent 384 bytes per sample, but the server uses only the first 128 (LLTF). 10 × 50/s × 400 B is heavy on a home network | The S3 sends **only the 128-byte LLTF block**: a third of the traffic. The queue grew 64 → 128 for 10 nodes | `csi_receiver_24` |
| The server's address had to be typed in (or found as `csi-pi.local`) | The server **announces itself**; receivers follow it if the laptop's IP changes | `server.py`, both receivers |
| The C5 had no fixed IP, no BSSID pin and no serial-stall fix (ORBI21's second DHCP server) | Same home-network handling as the S3; it also re-reads the router's MAC after roaming to another mesh box | `csi_receiver_5g` |
| The CSI byte order was only *checked* (S3 order) | It's now **detected per path**, and the server switches to the other common order (−32..31) if that's the one that fits. This covers the C5/C6. Training does the same | `csi_features.py`, `server.py`, `train_model.py` |
| `demo.sh` needs bash | `run_demo.py` works on Windows / macOS / Linux | `server/` |

### Accuracy (server side)

| Change | Effect on the simulator (see [Accuracy](#accuracy)) |
|---|---|
| **Common-mode removal** per receiver and band | Removes interference "ghosts": empty-room false alarms fell from ~20–60% to ~0–6% |
| **Interference gate** (no new person while all 2.4 GHz paths are disturbed together, unless 5 GHz agrees) | Removes the remaining false alarms → 0% in every benchmark run |
| **Hampel filter** on each 1 s window (outlier packets replaced by the median) | Cleaner AI features: held-out exact square 72% → 80% |
| Up to **10 transmitters** (room.json) | More paths: typical position error 1.6 m → ~1.0 m |
| 5 GHz router link used by the **AI model** and the interference gate | The model sees both bands |
| Fade weights fitted **per band**; the router link can count for geometry when the router is inside the room (`in_room: true`) | Correctness |

Things that were tried and **not** kept, because they didn't help on the benchmark:
- median / trimmed RSSI instead of mean / std: fewer false alarms, but more missed people;
- tightening the geometry's path width and averaging radius.

---

## Hardware

| Qty | Board | Role | Sketch | Arduino board setting |
|---|---|---|---|---|
| up to 10 | ESP32 DevKit / ESP32-C3 / **ESP32-C6 SuperMini** / ESP32-C5 | **Transmitters**, `NODE_ID` 1–10 | `firmware/csi_transmitter` | ESP32 Dev Module / ESP32C3 / ESP32C6 / ESP32C5 Dev Module |
| 1 | **ESP32-S3**, e.g. Seeed XIAO ESP32S3 with its antenna (a dipole is better) | **2.4 GHz receiver**, `RECEIVER_ID 1` | `firmware/csi_receiver_24` | XIAO_ESP32S3 (or ESP32S3 Dev Module) |
| 1 | **ESP32-C5 DevKit, 4 MB flash** | **5 GHz receiver**, `RECEIVER_ID 2` | `firmware/csi_receiver_5g` | ESP32C5 Dev Module, Flash 4MB |
| (optional) | another ESP32-C6 (or S3) | **2nd 2.4 GHz receiver**, `RECEIVER_ID 3`: the biggest accuracy upgrade (crossing paths) | `firmware/csi_receiver_24` | ESP32C6 Dev Module |
| 1 | Laptop / PC on the home WiFi | Runs `server/server.py` | – | Python 3.9+ |
| – | Your home router | Provides the WiFi, and is the 5 GHz "transmitter" for the C5 | – | – |

Arduino IDE: install **"esp32 by Espressif Systems" 3.3.0 or newer** (C5 support and CSI need it). Every sketch was compiled with core **3.3.12**, with zero warnings, for all the boards above.

---

## Channels and settings cheat sheet

### Your router (do this first)

| Setting | 2.4 GHz | 5 GHz |
|---|---|---|
| Channel | **Fixed** to **1, 6 or 11**, whichever is quietest (a WiFi analyzer app shows this). ORBI21 was on **1** on 2026-11-06 | **Fixed** to **36, 40, 44 or 48** if the router allows it. **Avoid 52–144 (DFS):** the router may jump off them for radar |
| Width | **20 MHz** | Anything (the C5 always uses 20 MHz) |
| Security | WPA2 or WPA2/WPA3 mixed (not WPA3-only) | same |
| Other | Not a guest network (guest networks block device-to-device traffic). Turn off "client isolation" / "AP isolation" | same |

With beacons, a fixed 2.4 GHz channel is no longer *required*: transmitters follow the receiver. It's still better, because every channel change costs about 10–15 s of data while the transmitters search.

### ORBI21 specifics (from flash-ready's testing)

- **Two DHCP servers.** One device hands out **172.27.35.x** behind a NAT that loses ~90% of packets. So:
  - the S3 uses the fixed IP **172.16.0.240**;
  - the C5 uses **172.16.0.241**;
  - the gateway is **172.16.0.1**.

  **The laptop must also be on 172.16.0.x.** Check with `ipconfig` (Windows) or `ip a` / `ifconfig`. If it got 172.27.35.x, reconnect, or give it a fixed 172.16.0.x address.
- **Orbi is a mesh:** several boxes have the same name. **Both 2.4 GHz receivers (if you have two) must be on the same box** so they share a channel: put the same `WIFI_BSSID` in both. The main router's 2.4 GHz BSSID was **0E:02:8E:9B:DA:18**. Each receiver prints the box it joined.

### Every board's `config.h`

| Board | Setting | Value |
|---|---|---|
| **Every transmitter** | `NODE_ID` | **1–10, different on every board.** Must match `transmitters` in `server/room.json` |
| | `FOLLOW_RECEIVER` | `1` (follow the receiver's beacons) |
| | `AUTO_CHANNEL_SSID` | `"ORBI21"` (fallback: copy the router's channel) |
| | `WIFI_CHANNEL` | `1` (last-resort fallback = the router's 2.4 GHz channel) |
| | `SEND_RATE_HZ` / `TX_POWER_DBM` / `SEND_JITTER_US` | `50` / `15` / `2000`, the **same on every transmitter** (use 40 Hz if 10 nodes feel heavy for your network) |
| **S3** (`csi_receiver_24`) | `WIFI_SSID` / `WIFI_PASSWORD` | `"ORBI21"` / the password (see [Security](#security-notes)) |
| | `STATIC_IP` / `STATIC_GATEWAY` | `"172.16.0.240"` / `"172.16.0.1"` (`""` = DHCP on a normal home network) |
| | `WIFI_BSSID` | `""`, or the main box's 2.4 GHz BSSID |
| | `RECEIVER_ID` | `1` |
| | `SERVER_IP` | `""` (found automatically; set the laptop's IP only if announcements are blocked) |
| | `BEACON_RATE_HZ` / `CSI_SEND_BYTES` | `5` / `128` |
| **C5** (`csi_receiver_5g`) | `USE_5GHZ` | `1` |
| | `WIFI_SSID` | `"ORBI21"` (or the 5 GHz name if your router splits them) |
| | `STATIC_IP` | `"172.16.0.241"` |
| | `RECEIVER_ID` | `2` |
| | `CSI_FROM_ACCESS_POINT` / `PING_RATE_HZ` | `1` / `50` |
| **Optional 2nd 2.4 GHz receiver** (`csi_receiver_24` on a C6) | `RECEIVER_ID` / `STATIC_IP` / `WIFI_BSSID` | `3` / `"172.16.0.242"` / **same box as the S3**. Then set `"enabled": true` for receiver 3 in `room.json` |
| **Laptop** (`server/server.py`) | `UDP_PORT` / `DISCOVERY_PORT` / `WEB_PORT` | `5005` / `5006` / `5000` (must match the receivers) |

Network ports, for firewalls: the laptop **receives UDP 5005** (CSI), **sends UDP broadcasts to 5006** (announcements), and **serves TCP 5000** (web pages).

---

## Setup, step by step

### 1. Router
Set the channels as in the [cheat sheet](#your-router-do-this-first). Note the 2.4 GHz channel.

### 2. Laptop
```bash
git clone -b home-wifi-dual-band https://github.com/Rohan578/RSSI-CSI-Demo-.git
cd RSSI-CSI-Demo-/server
pip install -r requirements.txt
python server.py
```
- **Windows:** when the firewall asks, allow Python on **Private networks**. Your home WiFi must be set to **Private** (Settings → Network → your WiFi → Network profile), otherwise incoming UDP is blocked.
- **macOS:** allow incoming connections for Python when asked.
- It prints `[DISCOVERY] Announcing this server (172.16.0.x)` and `[WEB] From a phone on the same WiFi: http://172.16.0.x:5000/room`.

Keep it running while you flash the boards: the receivers look for it at start-up.

### 3. Put the WiFi password into the receiver sketches
```bash
./tools/fill_wifi_password.sh          # Linux/macOS: copies the laptop's saved ORBI21 password
```
On Windows, type it into `WIFI_PASSWORD` in `firmware/csi_receiver_24/config.h` and `firmware/csi_receiver_5g/config.h`.

**Put the placeholder back before committing** (`./tools/fill_wifi_password.sh --clear`). The transmitters need no password.

### 4. Flash the S3 (Arduino IDE)
- Open `firmware/csi_receiver_24/csi_receiver_24.ino`. Four tabs should appear.
- Board: **XIAO_ESP32S3**, with **USB CDC On Boot: Enabled**.
- Upload, then open the Serial Monitor at 115200 baud. Check it shows `[SERVER] Laptop server found at ...`.

### 5. Flash the C5
- Open `firmware/csi_receiver_5g/csi_receiver_5g.ino`.
- Board: **ESP32C5 Dev Module**, **Flash Size 4MB**, **USB CDC On Boot: Enabled** (on the native USB port).
- Upload. Check it shows `BAND: 5 GHz` and `router | CSI/s ~50`.

### 6. Flash ONE transmitter first (`NODE_ID 1`)
- Open `firmware/csi_transmitter/csi_transmitter.ino` and set `NODE_ID 1`.
- Pick the board (for a C6 SuperMini: **ESP32C6 Dev Module, USB CDC On Boot Enabled**) and upload.
- It should say `Receiver heard on channel N - sending there`, then `LOCKED`.
- On the S3, `node 1 | CSI/s ~50` appears.

### 7. Flash the other transmitters
Change `NODE_ID` before every upload (2, 3, ... 10). **Label each board.**

### 8. Measure the room into `server/room.json`
Measure where every board and the router really are. Then restart `server.py`.

### 9. Calibrate
Everyone leaves the room. Open `http://<laptop>:5000/room` and press **Calibrate**. Do this again whenever furniture or boards move.

### 10. (Optional, best accuracy) Record volunteers and train the AI model
See [Collecting data](#collecting-data-and-training-the-ai-model).

---

## Placing the boards

The default `room.json` is a 20 × 20 ft room (6.10 m):

```
                         north wall
    TX 3 (0.25,5.20)   TX 4 (0.90)  TX 5 (2.25)  TX 6 (3.85)  TX 7 (5.20)   all at y = 5.85
          +---------------+---------------+---------------+   TX 8 (5.85,5.20)
          |      A3       |      B3       |      C3       |
    TX 2  +---------------+---------------+---------------+   TX 9 (5.85,3.40)
   (0.25, |      A2       |      B2       |      C2       |
    3.40) +---------------+---------------+---------------+   TX 10 (5.85,1.60)
    TX 1  |      A1       |      B1       |      C1       |
   (0.25, +---------------+---------------+---------------+
    1.60)   C5 (0.60,0.25)     S3 receiver (3.05, 0.25)
                         south wall          (home router: outside the room)
```

- **One 2.4 GHz receiver:** put it in the middle of one wall, and spread the transmitters along the other three walls. The paths then fan out over the whole floor.
- **All boards at the same height, about 1 m (chest height)**, so the direct paths pass through people's torsos.
- **The C5 measures the router → C5 path.** Put it where that path crosses the room if you can, for example on the wall *opposite* the router's direction.
  - If the router is inside the room, set `"in_room": true` in `room.json` and measure its position. Its path then also helps place people.
  - If the router is in another room, its path still helps the AI model and the interference check.
- **A 2nd 2.4 GHz receiver on the opposite wall** (`receivers.3`, north wall middle) gives crossing paths. With one receiver every path ends at the same board, so "how far along the path" is the weakest part.
- **Never move or rotate a board after calibrating or recording training data.** Antennas vertical, cables taped.

---

## What each Serial Monitor should say

**Transmitter (every 5 s):**
```
node 4 | LOCKED ch 1 | pkt/s 50.0 | receivers [1] | beacon age 120 ms | busy 0 | errors 0 | channel changes 1
```
- `SEARCH` = looking for a receiver (fast-blinking LED).
- `FALLBACK` = sending on the router's channel because no receiver was heard (double blink).
- `LOCKED` = normal (slow blink).

**S3 (every 5 s):**
```
[STATS] WiFi OK ch 1 | server 172.16.0.75 (announcement) | CSI 25031 | sent 25010 in 2780 UDP | queue full 0 | UDP errors 0 | no-server drops 0 | beacons 250
  node   1 | CSI/s  49.6 | heard/s  49.6 | RSSI  -52 dBm | CSI 128 bytes
  ... one line per transmitter ...
```

**C5 (every 5 s):**
```
[STATS] 5 GHz ch 36 | WiFi OK | server 172.16.0.75 (announcement) | CSI 2504 | ...
  router    | CSI/s  49.8 | pings ok/s  49.8 | RSSI  -58 dBm | CSI ... bytes | timeouts 0
```

---

## Accuracy

### Measured on the simulator (`server/benchmark.py`)

`benchmark.py` runs a fixed 8-minute scenario through the simulator and the **real analysis code**, and scores it against the true positions:
- an empty room;
- 1 person walking;
- 1 person standing still at 15 spots;
- 2 people walking.

The simulator has ESP32 quirks, and optional home-WiFi interference: bursts that hit every 2.4 GHz path at once, with corrupted subcarriers, inflated RSSI and lost packets.

Geometry mode (no AI model), averaged over 2 seeds. **"Interference" = home-WiFi bursts on 2.4 GHz.**

*(Results table being generated: re-run `python server/benchmark.py --all --baseline <flash-ready pi_server>`.)*

*err* = median distance between a shown person and the true position (metres; guessing the room's centre ≈ 2.3 m). *right square* = exact 3 × 3 zone. *2 people* = shown as exactly 2. *false alarm* = anyone shown while the room is empty.

**With the AI model** (trained on 2 simulated guided sessions, tested on a third):

| | flash-ready code | this branch |
|---|---|---|
| Held-out self-test: 1 person, exact square | 72% | **78–80%** |
| Held-out self-test: square or neighbour | 92% | **94%** |
| Empty room recognised | 98% | **100%** |
| Live scenario with interference: false alarms in the empty room | 11.6% | **0%** |
| Live scenario: 1 person counted right | 96.5% | **98.2%** |

**Read this honestly:**
- The simulator is a simplified room with simplified bodies. These numbers compare **versions and layouts fairly**; they don't predict your basement.
- Two people are still often shown as one: that's the limit of one 2.4 GHz receiver. A 2nd receiver raised "2 people counted right" the most.

To measure your real room, record the sessions in [`docs/VOLUNTEERS.md`](docs/VOLUNTEERS.md), then run `python train_model.py --test-session <held-out session>`.

Re-run the table yourself:
```bash
git worktree add ../flash-ready origin/flash-ready
python server/benchmark.py --all --baseline ../flash-ready/csi-arduino/pi_server
```

---

## Collecting data and training the AI model

Everything from `flash-ready` still works, now on the laptop:

1. Open **`/collect`** on a laptop, TV or phone in the room. It tells volunteers out loud which taped square to stand in, and labels the data itself. The briefing and a 1.5-hour plan are in [`docs/VOLUNTEERS.md`](docs/VOLUNTEERS.md).
2. Train **on the same computer that runs the server** (a model only loads with the scikit-learn version that trained it):
   ```bash
   cd server
   python train_model.py --list                     # sessions found in data/
   python train_model.py --test-session <id>        # honest test on a held-out session
   python train_model.py                            # final model from all sessions
   ```
   Then restart `server.py`, or load it without a restart:
   `python -c "import urllib.request as u; u.urlopen(u.Request('http://localhost:5000/api/model/reload', method='POST'))"`
3. The model now uses **every path, including the C5's 5 GHz router path** (`r2-t0-5`).

`FEATURE_VERSION` is now **2** (Hampel-filtered windows). Calibrations and models from `flash-ready` are refused with a clear message: calibrate again and re-run `train_model.py`.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Receiver: `WIFI_PASSWORD ... still the placeholder` | Fill in the password (step 3), upload again |
| Receiver stuck at `Connecting to "ORBI21"....` | Wrong password, the network is WPA3-only, or out of range. On 5 GHz: does the router have a 5 GHz network with this name? |
| Receiver: `[SERVER] Not found yet` and a fast-blinking LED | `server.py` isn't running, or the laptop is on another subnet (ORBI21: must be 172.16.0.x), or announcements are blocked (Windows network set to *Public*, or client isolation on the router). Workaround: set `SERVER_IP` to the laptop's IP |
| Receiver finds the server, but `/room` says "Waiting for WiFi data" | The laptop's firewall blocks incoming UDP 5005. Windows: allow Python on Private networks |
| Transmitter stays in `SEARCH` / `FALLBACK` | No 2.4 GHz receiver is up, or it's on a different mesh box out of range. Start the S3 first; the transmitter retries every 30 s by itself |
| S3: a node has `heard/s` but `CSI/s` 0 | Arduino core older than 3.3: update it |
| S3: `queue full` climbing | Too many samples for the S3's WiFi uplink: lower `SEND_RATE_HZ` to 40 on all transmitters, and keep `CSI_SEND_BYTES 128` |
| C5: `pings ok/s` 0 | The router doesn't answer pings: set `PING_TARGET_IP` to the laptop's IP |
| C5 data stops now and then | The router's 5 GHz is on a DFS channel (52–144) and jumped: fix it to 36–48 |
| `/room` chip says **"WiFi interference - filtering"** | Other 2.4 GHz traffic is disturbing every path. It's being subtracted; no new person appears until it passes, unless 5 GHz confirms. If it's constant, move the router's 2.4 GHz channel |
| Server log: `CSI uses the 'linear' byte layout - switched` | Normal for some C5/C6 firmware: the server adapted by itself |
| Server log: `CSI layout doesn't match any known one` | The receiver isn't running this branch's sketch (or its CSI settings were changed) |
| Positions jump around | Expected with one 2.4 GHz receiver. Add transmitters, add a 2nd receiver, train the AI model, and check no board or cable moves |
| `Model ... trained on older features` | You updated from `flash-ready`: calibrate again, re-run `train_model.py` |

---

## Repository layout

```
firmware/
  csi_transmitter/      ESP32 / C3 / C6 / C5 transmitter (2.4 GHz), follows the receiver's channel
  csi_receiver_24/      2.4 GHz receiver: ESP32-S3 (XIAO or DevKit), or ESP32-C6 as a 2nd one
  csi_receiver_5g/      5 GHz receiver: ESP32-C5 DevKit (4 MB), pings the router
                        (espnow_packet.h is identical in all three; csi_packet.h in both receivers)
server/                 runs on the laptop (was pi_server/)
  server.py             UDP receiver, discovery announcements, CSV logger, web pages + API
  analysis.py           live engine: calibration, common-mode removal, interference gate, tracking
  localizer.py          room geometry, path-geometry estimate, tracker, AI-model loader
  csi_features.py       CSI -> features (Hampel filter, byte-layout detection); shared by live + training
  train_model.py        trains the zone + head-count random forests from guided sessions
  guided.py             volunteer schedule + automatic labels (/collect)
  sim_room.py           simulated room: multiple receivers, 5 GHz router link, interference
  benchmark.py          accuracy benchmark (+ simulated guided sessions for training tests)
  bench/                room layouts used by the benchmark
  run_demo.py           server + simulator, any OS (demo.sh = the same for bash)
  compare_bands.py      2.4 vs 5 GHz comparison of a recording
  fake_receiver.py      minimal fake receiver for quick tests
  room.json             YOUR room: size, zones, board and router positions
  static/               3D view (three.js included), volunteer page
docs/VOLUNTEERS.md      volunteer briefing and data plan
tools/fill_wifi_password.sh   puts the WiFi password into the receiver configs (and --clear)
.githooks/pre-commit    refuses commits with a real WiFi password
```

---

## Security notes

- **WiFi passwords never go into git.** The receiver `config.h` files hold the placeholder `__WIFI_PASSWORD__`. Enable the guard once per clone:
  ```bash
  git config core.hooksPath .githooks
  ```
  It refuses commits where `WIFI_PASSWORD` isn't the placeholder.
- **The dashboard has no login.** Anyone on the home WiFi can open it.
- **ESP-NOW packets are unencrypted broadcasts** carrying only a node ID and a counter. Nothing personal is ever sent or stored: no cameras, no audio, and recordings are CSI numbers only.

---

## Credits

- Espressif's **esp-csi** examples (`csi_send`, `csi_recv`, `csi_recv_router`): [github.com/espressif/esp-csi](https://github.com/espressif/esp-csi).
- **three.js** (MIT), bundled in `server/static/vendor/three`.
- The `flash-ready` branch's server, 3D view, guided collection, simulator and hardware notes, on which this branch builds.

# Raspberry Pi 5: 3D room view, volunteers and AI model

This document covers this setup: linksys222 on the Aruba AP, 5 ESP32 transmitters and 1
ESP32-S3 receiver, all on 2.4 GHz, in a 20 × 20 ft basement.

```
 5 x ESP32 transmitters --ESP-NOW (2.4 GHz ch 11)--> ESP32-S3 receiver (192.168.1.60)
                                                          | UDP 5005 over linksys222
 Aruba AP (192.168.1.10, no uplink) <---------------------+
          | 5 GHz                                         v
          +-------------------------------------->  Raspberry Pi 5 (192.168.1.50)
                                                     server.py: logging, analysis,
 laptop --ethernet (10.42.0.x)--> Pi                 dashboard on port 5000
```

> **Current network (2026-11-06): the home WiFi ORBI21.** linksys222 went down.
> - The Pi is `csi-pi.local` (`ssh csi-pi`, dashboard `http://csi-pi.local:5000/room`), set up
>   with `deploy/use_home_wifi.sh`.
> - The S3 finds the Pi by that name; the transmitters follow ORBI21's channel.
> - The Pi's DHCP server never runs on ORBI21.
>
> Details: the top-level README, "Network design". The linksys222 notes below still apply if
> you switch back.

## Addresses

| Device | linksys222 | Notes |
|---|---|---|
| Aruba AP | 192.168.1.10 | Static. Admin page at `https://192.168.1.10:4343` (login in `notes.txt`) |
| Raspberry Pi | **192.168.1.50** | Static, set by `setup_pi.sh`. Ethernet to the laptop: `10.42.0.222` |
| ESP32-S3 receiver | **192.168.1.60** | Static, set in `csi_receiver_s3/config.h` |
| Phones and laptops | 192.168.1.100–199 | Handed out by the Pi's DHCP server, which serves linksys222 only |

**linksys222 has no router and no DHCP server of its own.** This laptop's attempt to join
it at 21:12 on Nov 4 associated fine, then timed out waiting for an address. That's why:

- the S3 now uses a fixed IP; the original sketch would wait forever for an address;
- the Pi hands out addresses to anything else that joins.

## Bring-up, in order

1. **Fill in the WiFi password** (on the laptop): `./deploy/fill_wifi_password.sh`.
   - It copies the linksys222 password this laptop already has saved, or asks you for it.
   - Don't push `config.h` afterwards. `--clear` restores the placeholder.
2. **Flash the ESP32s** (Arduino IDE, esp32 core 3.3+).
   - **S3:** flash `csi_receiver_s3`.
   - **Transmitters:** flash `csi_transmitter` five times, with `NODE_ID` set to 1, 2, 3,
     4 and 5. Label each board.
   - Each transmitter now scans for linksys222 and copies its channel at boot. It prints
     `[RADIO] Found "linksys222" on channel 11`.
3. **Prepare the Pi's SD card**, in a USB reader on the laptop, with
   `./deploy/prepare_sd_card.sh /run/media/$USER/bootfs`. This gives the Pi:
   - a user `csi` (your SSH key only);
   - SSH on;
   - linksys222 at 192.168.1.50, set up at first boot by cloud-init.

   Alternatively, with a keyboard and screen: `sudo systemctl enable --now ssh`.
4. **Deploy** (on the laptop, Pi on the ethernet cable): `./deploy/deploy_to_pi.sh`. This:
   - installs your SSH key (`~/.ssh/id_ed25519.pub`);
   - copies the code to `~/csi`;
   - installs packages while the Pi still has internet through the laptop;
   - joins the Pi to linksys222 at 192.168.1.50;
   - starts the DHCP server and the `csi-server` service;
   - adds `ssh csi-pi` and `ssh csi-pi-eth` shortcuts to `~/.ssh/config`.
   - Later code updates: `./deploy/deploy_to_pi.sh --code`.
5. **Pin the AP's channels.** Run `./deploy/ap_tunnel.sh`, then follow `deploy/AP_SETUP.md`.
   Power-cycle the transmitters afterwards.
6. **Place the boards** as in `VOLUNTEERS.md`, then measure and edit `pi_server/room.json`.
7. **Open the 3D view.**
   - Over the cable: `http://10.42.0.222:5000/room`.
   - On linksys222: `http://192.168.1.50:5000/room`.
   - **Calibrate** with the room empty.
8. **Collect training data** at `/collect` (`VOLUNTEERS.md`), then **train** on the Pi
   (`train_model.py`).

### When you unplug the ethernet cable

This laptop has **one WiFi card**. To reach the Pi without the cable, the laptop has to
join linksys222, and that drops ORBI21 and your internet until you switch back. Then:

```bash
nmcli connection up linksys222     # the normal profile (gets 192.168.1.1xx from the Pi)
ssh csi-pi                         # = csi@192.168.1.50, with your key
nmcli connection up ORBI21         # back to the internet afterwards
```

**Never use the laptop's other profile, `linksys222 1`.** Its IPv4 method is "shared",
which starts the laptop's own DHCP server on linksys222. Two DHCP servers would fight. A
USB WiFi adapter would let the laptop stay on both networks.

## Pages

| URL | What it shows |
|---|---|
| `/room` | **3D view.** The basement slowly spins. You see the boards, the radio paths (brighter when disturbed), a floor glow where people probably are, and a figure per person. Drag to look around; spinning resumes after 8 s. |
| `/collect` | **Volunteer guide**, for a TV, laptop or phone in the room. It shows and speaks who goes to which square, and labels the data itself. |
| `/` | The original per-link dashboard. It now also has a calibrate button and links to the pages above. |

## How people are found

1. **Calibration.** Record ~20 s of the **empty** room, using the button on `/room` or the
   first step of every `/collect` session. Everything is measured against it. Redo it when
   furniture moves, or when the view shows people in an empty room.
2. **Path geometry** works right away, with no training.
   - It uses each path's change in signal strength (RSSI) relative to the empty room. A
     body near the direct path changes it by a few dB.
   - It picks the floor spot that best explains which paths changed.
   - Paths stuck in a multipath fade are weighted down, because they react to people
     anywhere.
   - **Limits:** there is only one receiver, so every path ends at the S3. Side-to-side
     position is better than position along a path. It is unreliable for two or more
     people and usually shows one.
3. **AI model**, after `train_model.py` has run.
   - A random forest learns the full CSI "fingerprint" of each square from the volunteer
     sessions. The fingerprint is the 52-subcarrier shape plus wobble for every path,
     relative to the empty room.
   - It answers yes or no per square, so several people work.
   - The geometry estimate is then only used to place each person inside their square.

**Measured on the simulator (`sim_room.py`), NOT on your basement.** The AI model was
trained on two simulated guided sessions, then tested on people wandering freely:

| | Path geometry (no training) | AI model |
|---|---|---|
| Empty room recognised | yes | 100% |
| 1 person: position error | ~1.4–1.7 m | ~1.1–1.5 m (median ~1.0–1.3 m) |
| 1 person: count right | ~75% | ~80–86% |
| 2 people | usually shown as 1 | usually shown as 1 (count right ~12%) |
| Self-test, standing in squares | – | exact square 54%, square or neighbour 84% |

Real rooms differ: real walls, real bodies, and real ESP32 noise. Train and test on real
sessions (`--test-session`) to get your own numbers.

The first CSI from each path is also checked for the expected 802.11 byte layout. If the
"guard" subcarriers aren't empty, the log and `/room` show a warning instead of silently
computing nonsense.

### Why not a ready-made AI "WiFi to 3D people" model? (checked 2026-11-04)

None of the published models can run on this hardware without retraining in this room:

- **DensePose-from-WiFi** (CMU) and **Person-in-WiFi 3D** (CVPR 2024) need research WiFi
  cards with 3 antennas each, and several receivers.
- The ESP32 has **one antenna**, about 52 usable subcarriers, and no stable phase.
- **RuView / wifi-densepose** does the ESP32 data capture for real. Its pose-estimation
  claims have not been reproduced by independent reviewers.
- **gavmc/CSI** (ESP32 plus a webcam teacher) reports no trustworthy accuracy yet, and
  releases no model.

The approach that does work is the one here: **train on your own room** with labelled
volunteer data. A future upgrade would be a webcam "teacher": record video alongside the
CSI, let a pose model label exact positions, and train on those instead of squares.

## Testing without hardware

```bash
cd pi_server
python3 server.py                       # terminal 1
python3 sim_room.py                     # terminal 2: 0, 1, 2, 3... simulated people
python3 sim_room.py --follow-collect    # or: simulated volunteers that follow /collect
```

The 3D view then shows the true simulated positions as white outlines, and scores itself
under "Simulator check".

## Files (pi_server/)

| File | Purpose |
|---|---|
| `server.py` | UDP receiver, CSV logger, all web pages and APIs (extended from the original) |
| `analysis.py` | Live engine, 4 updates/s: calibration, per-path scores, localisation, tracking |
| `localizer.py` | Room geometry, path-geometry estimate, people tracker, AI-model loader |
| `csi_features.py` | CSI → features. Shared by live and training, so both see the same numbers |
| `guided.py` | Volunteer schedule and automatic labels |
| `train_model.py` | Trains `models/zone_model.joblib`. **Run it on the Pi**, since a model only loads with the scikit-learn version that trained it |
| `sim_room.py` | Simulated basement with people |
| `room.json` | Room size, squares, board positions. **Edit it to match reality** |
| `static/` | 3D view and volunteer page. three.js is included, because linksys222 has no internet |
| `data/` | Recordings `csi_*.csv`, `calibration.json`, `sessions/*.json` |

## Troubleshooting

| What you see | Fix |
|---|---|
| S3 Serial Monitor: `WIFI_PASSWORD ... still the placeholder` | Run `deploy/fill_wifi_password.sh`, then upload again |
| S3 connects, but the 3D view says "Waiting for WiFi data" | `ssh csi-pi`, then `journalctl -u csi-server -f`. Is the Pi on linksys222 (`nmcli device`)? Check *Deny inter-user bridging* on the AP |
| A transmitter prints `"linksys222" not found - using WIFI_CHANNEL 11` | The AP was off when it booted. Power-cycle it once the AP is up |
| People shown in an empty room | Re-calibrate with the room really empty, and the door the way it usually is |
| "Calibration looked busy" | Someone was inside or walking past. Calibrate again |
| `/room` is blank on an old phone or tablet | It needs WebGL and a recent browser. Try a laptop |
| `train_model.py`: "zones visited fewer than 2 times" | Record more rounds |
| The model won't load: "scikit-learn version" | You trained on another machine. Re-run `train_model.py` on the Pi |
| Times in the CSV are off | linksys222 has no internet, so the Pi can't sync its clock. Plug the ethernet in for a minute, or add an RTC battery to the Pi 5 |

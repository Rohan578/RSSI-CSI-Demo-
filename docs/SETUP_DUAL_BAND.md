# Dual-band setup sheet

**2.4 GHz:** ESP32 / C6 / C3 transmitters → **S3 receiver**
**5 GHz:** C5 transmitters + the access point → **C5 receiver**
Both receivers → **Raspberry Pi 5**

Print this, fill in the blanks, and keep it with the hardware.

---

## 1. Access point (do this first)

| Setting | 2.4 GHz network | 5 GHz network |
|---|---|---|
| Name (SSID) | ______________ | ______________ |
| Channel (fixed, not Auto) | 1, 6 or 11 → **____** | 36, 40, 44 or 48 → **____** |
| Width | 20 MHz | 20 MHz |
| Security | WPA2 | WPA2 |

- Turn off band steering / "smart connect", so the two networks stay separate.
- Give the Pi a DHCP reservation. Pi IP: **______________**
- Don't move the access point after you start collecting data. It's a 5 GHz sender.

## 2. Pi

```bash
cd pi_server
pip install -r requirements.txt
python3 server.py
```

Dashboard: `http://<pi-ip>:5000`

## 3. Receivers

| Board | Sketch | `config.h` settings |
|---|---|---|
| **S3** (dipole antenna) | `csi_receiver_s3` | `WIFI_SSID` = 2.4 GHz name · `PI_SERVER_IP` · `RECEIVER_ID 1` |
| **C5** | `csi_receiver_c5` | `USE_5GHZ 1` · `WIFI_SSID` = 5 GHz name · `PI_SERVER_IP` · `RECEIVER_ID 2` · `CSI_FROM_ACCESS_POINT 1` · `CSI_FROM_TRANSMITTERS 1` |

After uploading, check each receiver's Serial Monitor prints the channel you set on the access point.

## 4. Transmitters

One sketch for all of them: `csi_transmitter`. Change `NODE_ID` before **every** upload.

| NODE_ID | Chip | Band | `USE_5GHZ` | `WIFI_CHANNEL` | Position in room |
|---|---|---|---|---|---|
| 1 | ESP32 | 2.4 | 0 | (2.4 ch) | |
| 2 | ESP32 | 2.4 | 0 | (2.4 ch) | |
| 20 | C6 | 2.4 | 0 | (2.4 ch) | |
| 21 | C6 | 2.4 | 0 | (2.4 ch) | |
| 40 | C3 | 2.4 | 0 | (2.4 ch) | |
| 60 | C5 | **5** | **1** | (5 ch) | |
| 61 | C5 | **5** | **1** | (5 ch) | |
| | | | | | |

Use the same values on every transmitter: `SEND_RATE_HZ 50` and `TX_POWER_DBM 15`.

## 5. Placement

- S3 and C5 receivers **side by side** on one wall.
- Transmitters along the **opposite walls and corners**, so their paths cross the area people move through.
- Put each **C5 transmitter next to a 2.4 GHz transmitter**. That pair then measures nearly the same path on both bands, which is what makes the comparison meaningful.
- Keep 5 GHz paths shorter and unobstructed. 5 GHz is blocked more by people and furniture.
- **Never move or rotate a board** once you start collecting. The C5's PCB antenna is sensitive to orientation.

## 6. Check before recording

| Where | What you should see |
|---|---|
| S3 Serial Monitor | One line per 2.4 GHz node, about 50 CSI/s each |
| C5 Serial Monitor | `access pt` at about 50 CSI/s, plus one line per C5 transmitter at about 50 CSI/s |
| Dashboard | One card per link, plus the band row: "2.4 GHz: N links · 5 GHz: M links" |
| Dashboard, room empty | Band check says **"Bands agree"**, with low motion on both bands |

**Bring-up order for 5 GHz:** start with just the C5 receiver and **one** C5 transmitter. Confirm that node shows about 50 CSI/s, then add the rest.

## 7. After a recording session

```bash
python3 compare_bands.py
```

This compares the bands second by second. Windows where only 2.4 GHz sees movement (likely interference) are marked `use_for_training = 0` in `data/csi_<date>_<time>_bands.csv`. It also prints the average motion per label for each band, so you can see which band reacts more to each activity.

# RSSI / CSI Demo

Wi-Fi channel state information (CSI) sensing with two ESP32 boards and a computer, all on the same existing Wi-Fi network (a normal 2.4 GHz router).

```
 ESP32-C6 ──ESP-NOW broadcast, HT20 MCS0, ~100 Hz──▶ ESP32-S3 ──UDP, ADR-018 CSI frames (via router)──▶ Computer
```

- The **C6** broadcasts a tiny ESP-NOW packet every 10 ms. The packet contents don't matter; the S3 measures CSI from the radio frame itself. The C6 is forced to HT20 MCS0, because ESP-NOW's default 1 Mbps 802.11b rate carries no CSI.
- The **S3** captures CSI from those frames, learns the C6's MAC from the first packet and discards CSI from any other transmitter. It forwards each frame to the computer over UDP in the ADR-018 binary format.
- ESP-NOW needs both boards on the same channel, so both join the same router. No IP traffic is exchanged between them.
- The **computer** runs `csi_receiver.py` to receive, check and log those frames.

## Files

| File | Purpose |
|---|---|
| `csi_receiver.py` | Computer-side UDP receiver. Parses frames, prints live stats, optionally logs to CSV. |
| `esp32_CSI_sketch.ino` | S3 firmware: captures CSI from the C6's ESP-NOW packets and streams ADR-018 frames over UDP. |
| `esp32c6_espnow_sender.ino` | C6 firmware: broadcasts ESP-NOW packets at ~100 Hz as the CSI transmitter. |

## Flashing the boards

Arduino IDE with "esp32 by Espressif Systems" 3.x.

1. **Both sketches:** set `WIFI_SSID` / `WIFI_PASSWORD` to the *same* 2.4 GHz network.
2. **S3:** set `AGGREGATOR_IP` to your computer's IP on that network (`ipconfig getifaddr en0` on macOS). Board: "ESP32S3 Dev Module".
3. **C6:** board "ESP32C6 Dev Module", USB CDC On Boot "Enabled".
4. Power both boards, then run the receiver. The S3's serial monitor should print `[ESPNOW] Learned C6 MAC ...` once the C6 is up. If it keeps saying no packets from the C6, check the network and channel. Type `status` for counters.

## Running the receiver

Requires Python 3.8+. No dependencies.

```bash
python3 csi_receiver.py                      # listen on 0.0.0.0:5005
python3 csi_receiver.py --csv logs/run1.csv  # also append every frame to a CSV
python3 csi_receiver.py --dump               # print every frame
python3 csi_receiver.py --port 6000 --interval 2
```

Output is one line per node per interval:

```
[15:05:38] node 1 |   42.8 frames/s | loss  4.4% | rssi  -48.0 dBm | 64 subcarriers | mean amp  14.1
```

`loss` is computed from gaps in the frame sequence number. Ctrl-C prints a summary.

Your computer's firewall must allow incoming UDP on the chosen port (on macOS, allow Python when prompted). Check that the S3's `AGGREGATOR_IP` / `AGGREGATOR_PORT` point at this computer.

### CSV format

One row per frame. The CSI is stored raw in the `iq` column as space-separated int8 values in `imag, real, imag, real, ...` order, one pair per subcarrier.

```
recv_time,src_ip,node_id,seq,freq_mhz,rssi,noise,n_antennas,n_subcarriers,truncated,iq
```

`truncated` is `1` when the payload was shorter than the header declared (the sketch caps its buffer).

Reading it back:

```python
import csv, math
for row in csv.DictReader(open("logs/run1.csv")):
    v = list(map(int, row["iq"].split()))
    amp = [math.hypot(v[i], v[i + 1]) for i in range(0, len(v) - 1, 2)]
```

`csi_receiver.parse_packet()` returns a `CsiFrame` with `amplitudes()` and `phases()` helpers, so you can import it into analysis code.

## ADR-018 frame format

Little-endian. 20-byte header, then the CSI payload.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0xC5110001` |
| 4 | 1 | node_id |
| 5 | 1 | n_antennas |
| 6 | 2 | n_subcarriers |
| 8 | 4 | freq_mhz |
| 12 | 4 | sequence |
| 16 | 1 | rssi (int8, dBm) |
| 17 | 1 | noise floor (int8, dBm) |
| 18 | 2 | reserved |
| 20 | n_antennas × n_subcarriers × 2 | int8 (imaginary, real) pairs |

## Status

- [x] Receiver: parse, stats, CSV logging (tested against a simulated S3 sender)
- [x] S3 firmware: ESP-NOW receive, MAC learning and filter, CSI queue (no frame dropping), larger buffers. Written, **not yet compiled or run on hardware**
- [x] C6 firmware: ESP-NOW broadcast at ~100 Hz, HT20 MCS0. Written, **not yet compiled or run on hardware**
- [ ] Set real Wi-Fi settings and `AGGREGATOR_IP`, flash, and verify end to end
- [ ] Signal processing and visualisation (amplitude filtering, motion/presence detection)

## Notes

- The S3 reports about 128 "subcarriers" per frame: the 64 from the legacy training field followed by the 64 from the HT training field, concatenated. Packets using STBC carry 192. Treat the two halves as separate measurements when analysing.
- The C6 only supports 2.4 GHz Wi-Fi.
- The sketches contain Wi-Fi credentials. Don't commit real ones to a public repo.

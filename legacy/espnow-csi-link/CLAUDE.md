# CLAUDE.md

Wi-Fi CSI sensing demo: an ESP32-C6 transmits, an ESP32-S3 captures CSI and forwards it over UDP to a computer running `csi_receiver.py`. See [README.md](README.md) for the overview and usage.

## Layout

- `csi_receiver.py`: computer-side receiver. Python stdlib only, must stay compatible with Python 3.9 (use `from __future__ import annotations` / `typing.Optional`, not runtime `X | None`).
- `esp32_CSI_sketch.ino`: S3 firmware (Arduino-ESP32 v3.x).
- `esp32c6_espnow_sender.ino`: C6 firmware (Arduino-ESP32 v3.x).

## Topology

All three devices are on the same existing Wi-Fi network (the router is the AP); neither board is an access point. The C6 broadcasts ESP-NOW packets at ~100 Hz. The S3 captures CSI from them and forwards ADR-018 frames over UDP to the computer on port 5005.

Design points that are easy to break:

- **ESP-NOW must be sent at HT20 MCS0.** The default 1 Mbps 802.11b rate has no OFDM training fields, so the S3 would get no usable CSI. `setupEspNow()` in the C6 sketch sets this, using `esp_now_set_peer_rate_config()` on IDF >= 5.4 and `esp_wifi_config_espnow_rate()` before that.
- **Both boards must be on the same channel.** That is why the C6 joins the router even though it sends no IP traffic.
- **The S3 filters CSI by transmitter MAC** (`info->mac`), learned from the first valid ESP-NOW packet (magic `0xC6E50001`) or pinned with `TX_MAC`. Without it, any frame the radio hears becomes CSI.
- **`EspNowPayload` is duplicated** in both sketches and must stay identical.
- **Modem sleep is off** (`WiFi.setSleep(false)`) on both boards so the S3 doesn't miss frames.
- The S3 hands CSI from the Wi-Fi task to `loop()` through a FreeRTOS queue (`csiQueue`). No Serial or UDP calls in the callbacks.
- Don't pass sketch-defined structs as parameters of top-level functions. The Arduino build hoists auto-generated prototypes above the struct definitions.

The README status checklist is the source of truth for what has been tested. The sketches have not been compiled or run on hardware yet (no Arduino toolchain on the dev machine).

## ADR-018 wire format

20-byte little-endian header (`struct` format `<IBBHIIbbBB`): magic `0xC5110001`, node_id, n_antennas, n_subcarriers, freq_mhz, sequence, rssi, noise, 2 reserved. Then `n_antennas * n_subcarriers * 2` int8 values.

- Per-subcarrier order is **(imaginary, real)**, the ESP-IDF convention. Amplitude is unaffected by this, but phase is. The sketch comment says "I, Q", so check there if phase ever looks wrong.
- The S3 sketch caps its payload buffer, so a frame can carry fewer bytes than the header declares. The receiver keeps the whole pairs it got and flags `truncated`.
- With the merged LLTF + HT-LTF config, an HT20 frame is ~128 "subcarriers" (64 + 64 concatenated), or 192 with STBC. The S3 buffers are sized for 192, so frames are no longer truncated.
- If you change the format, update the header layout in `csi_receiver.py`, the table in the README, and the sketch together.

## Conventions

- Keep the receiver dependency-free. Plotting or analysis goes in a separate script or an opt-in extra, not in the core receiver.
- Logs (`*.csv`, `logs/`) are gitignored.
- Don't commit real Wi-Fi credentials. Existing sketches contain some already.

## Testing the receiver without hardware

There's no test suite. To check changes, run `python3 csi_receiver.py --dump` and send hand-built frames from a throwaway script, with `struct.pack("<IBBHIIbbBB", 0xC5110001, node, n_ant, n_sc, freq, seq, rssi, noise, 0, 0) + payload` to `127.0.0.1:5005`. Cover a sequence gap (loss counted), a truncated payload, and a non-ADR-018 packet (dropped with a warning).

When piping output to a file, run Python with `-u`, otherwise stats lines are buffered.

## Open issues in the firmware

- Neither sketch has been compiled or run on hardware. Most likely compile problems: the ESP-NOW rate API names under `ESP_IDF_VERSION` in the C6 sketch, and the `esp_now_recv_info_t` callback signature in the S3 sketch (IDF 5.x).
- S3 `AGGREGATOR_IP` is `172.21.240.1`, which looks like a virtual adapter address rather than the computer's LAN IP. The two sketches also still have different Wi-Fi networks configured. Both need setting before use.
- The C6 sketch doesn't use an ESP-NOW send callback (its signature changed across IDF 5.x). `sendErrors` only counts `esp_now_send()` failures, and broadcast frames get no acknowledgement.
- The S3 keeps promiscuous mode on, so `csiFiltered` counts every frame it hears. If that load turns out to be a problem, test with promiscuous mode off, since ESP-NOW broadcast frames reach the S3 without it.

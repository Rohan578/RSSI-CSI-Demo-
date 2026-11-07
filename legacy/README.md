# legacy/: old experiments, do NOT flash these

These are the files that used to sit at the top of the repository: early ESP32 sketches,
and `csi-arduino.zip` as it was first uploaded. They are kept only for reference.

**The current code is in [`../firmware/`](../firmware/), [`../pi_server/`](../pi_server/) and
[`../laptop_server/`](../laptop_server/).** To flash the boards, follow
[`../docs/FLASHING.md`](../docs/FLASHING.md).

The WiFi passwords that were written in some of these files have been replaced with
`__WIFI_PASSWORD__`. They are still in the git history from before, so any network whose
password appeared here should get a new password.

## What else is here

| Folder / file | Was | What it is |
|---|---|---|
| top-level files and `csi-arduino.zip` | the `main` branch (first uploads) | Early sketches and the original firmware zip |
| [`espnow-csi-link/`](espnow-csi-link/) | the `espnow-csi-link` branch | The first ESP-NOW CSI link: a C6 sender, an S3 receiver streaming ADR-018 frames, and a PC receiver (`csi_receiver.py`). See its README |

The earlier `pi-3d-dashboard` and `csi-pi-dashboard` branches held older copies of the code in
`../pi_server/`, `../deploy/` and `../firmware/pi/`; this branch (`pi-csi-dashboard`) has all of it, updated, so they were removed.

The repository now has two branches: **`pi-csi-dashboard`** (this one, Raspberry Pi version) and
**`home-wifi-dual-band`** (home WiFi + laptop, 10 transmitters, ESP32-C5 on 5 GHz).

# Aruba AP: pin the channels (linksys222)

CSI sensing breaks if the access point changes channel on its own, so fix both radios to a
channel. The transmitters look up linksys222's channel **when they power up**, so after any
channel change, **power-cycle all 5 transmitters**. The S3 follows the AP by itself.

**Plan:** 2.4 GHz on channel **11**, 20 MHz wide. 5 GHz on channel **157**. These are the
channels linksys222 was already using on 2026-11-04, and channel 11 had no other network
on it in the scan. ORBI21 is on channels 1 and 48, so neither of these overlaps it.

## Reaching the AP's web page

The AP (`192.168.1.10`) can only be reached over linksys222. To avoid moving the laptop off
ORBI21, go through the Pi:

```bash
./deploy/ap_tunnel.sh        # needs setup_pi.sh done (Pi joined to linksys222)
# then open https://localhost:4343 in a browser and accept the certificate warning
```

Log in with the admin user and password from `notes.txt` (repo root).

A phone or laptop joined to linksys222 can also open `https://192.168.1.10:4343` directly,
once the Pi's DHCP server is running.

If nothing answers on 4343, try `http://localhost:8080`. If neither works, the AP's
management address isn't on the WiFi side; see "If the page won't load" below.

## Settings

Names differ slightly between Aruba Instant versions. Look for these:

1. **Fix the channels.** Go to Configuration → Access Points, select the AP, then
   **Edit → Radio**.
   - **2.4 GHz:** choose *Administrator assigned*, channel `11`, and a fixed transmit
     power (for example 15–18 dBm).
   - **5 GHz:** choose *Administrator assigned*, channel `157`.
     - Plain `157` means 20 MHz. `157+` or `157E` mean 40 or 80 MHz.
     - 40/80 MHz is fine for the Pi's own link. If the C5 will later sense on 5 GHz,
       use 20 MHz.
2. **Keep 2.4 GHz at 20 MHz.** Go to Configuration → RF → ARM and set *Wide channel bands*
   to `5GHz` or `None`, never `2.4GHz` or `All`.
3. **Stop the AP leaving its channel to scan.** In Configuration → RF, turn *Scanning* off.
   Scanning makes the AP hop away briefly, which shows up as gaps in the CSI.
4. **Let clients talk to each other.** The S3 → Pi traffic depends on these:
   - Configuration → System → General: *Deny inter-user bridging* = **Disabled**.
   - Networks → linksys222 → Advanced: *Broadcast filtering* = `ARP` or `Disabled`, not `All`.
   - Networks → linksys222 → Access: *Enforce DHCP* = **off**. The S3 uses a fixed
     address, so it never asks for a lease.
5. Band steering can stay on. The S3 is 2.4 GHz only, and the Pi prefers 5 GHz.

Save the configuration, then:

- Power-cycle all 5 transmitters.
- Check the S3's Serial Monitor. It should show `ACCESS POINT CHANNEL: 11`, and each
  node at about 50 CSI/s.

## Command-line alternative

If SSH is enabled on the AP:

```
ssh admin@192.168.1.10          # through the Pi: ssh -J csi@10.42.0.222 admin@192.168.1.10
g-channel 11 18                 # 2.4 GHz: channel 11, 18 dBm   (per-AP command)
a-channel 157 18                # 5 GHz:  channel 157, 18 dBm
```

Check these against your firmware's CLI reference (`show ap-env`, `show radio config`) before
relying on them.

## If the page won't load

Through the tunnel, `curl -k https://localhost:4343` should return HTML. If it doesn't, the
AP's management IP may be on a different subnet or VLAN from the WiFi clients. Two options:

- Connect any computer by cable to the PoE injector's data port and give it a static IP of
  `192.168.1.20/24`.
- Use the console port.

## Changing the 2.4 GHz channel later

Pick 1, 6 or 11, whichever is quietest. ORBI21 sits on 1, which rules out 1.

1. Change it on the AP.
2. Power-cycle the transmitters. Each one's Serial Monitor prints
   `[RADIO] Found "linksys222" on channel N`.
3. Record a new empty-room calibration.
4. Train again from new sessions. A model trained on one channel doesn't carry over to
   another.

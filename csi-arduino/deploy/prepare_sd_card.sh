#!/usr/bin/env bash
# ============================================================================
#  prepare_sd_card.sh  -  set up a Raspberry Pi OS SD card from the LAPTOP
#
#  For Raspberry Pi OS "trixie" images, which are configured at first boot by
#  cloud-init from files on the boot partition. No sudo needed: only the
#  boot partition (FAT) is written.
#
#     udisksctl mount -b /dev/sdX1          # the card's "bootfs" partition
#     ./deploy/prepare_sd_card.sh /run/media/$USER/bootfs
#     udisksctl unmount -b /dev/sdX1 && udisksctl power-off -b /dev/sdX
#
#  On its next boot the Pi then has:
#   - a user "csi": login with your SSH key only, sudo without a password
#     (set a password later on the Pi with:  sudo passwd csi)
#   - SSH turned on
#   - WiFi: joins linksys222 with the FIXED address 192.168.1.50, never as
#     the internet route (the ethernet cable to the laptop stays the way out)
#   - the WiFi country set (US) so the radio is allowed on
#  The WiFi password is wiped from the boot partition after it's been used.
#
#  WiFi password: $CSI_WIFI_PSK if set, otherwise you're asked.
#  SSH keys allowed to log in: KEYS="key1.pub key2.pub" (default: ~/.ssh/id_ed25519.pub).
#  A key WITHOUT a passphrase (e.g. one made for scripts/agents) lets tools log in unattended.
#  Re-running is fine: a new cloud-init instance id makes the Pi apply the
#  files again on its next boot.
# ============================================================================
set -euo pipefail
BOOT="${1:?usage: $0 <mounted boot partition, e.g. /run/media/$USER/bootfs>}"
PI_USER="${PI_USER:-csi}"
KEYS="${KEYS:-${KEY:-$HOME/.ssh/id_ed25519.pub}}"
SSID="${SSID:-linksys222}"
PI_IP="${PI_IP:-192.168.1.50}"
PREFIX="${PREFIX:-24}"
COUNTRY="${COUNTRY:-US}"

[ -f "$BOOT/cmdline.txt" ] && [ -f "$BOOT/user-data" ] && [ -f "$BOOT/meta-data" ] || {
  echo "$BOOT doesn't look like a Raspberry Pi OS (cloud-init) boot partition."; exit 1; }
KEY_LINES=""
for k in $KEYS; do
  [ -f "$k" ] || { echo "SSH public key $k not found (set KEYS=...)"; exit 1; }
  KEY_LINES+="      - $(head -n1 "$k")"$'\n'
done

PSK="${CSI_WIFI_PSK:-}"
if [ -z "$PSK" ]; then read -rsp "$SSID WiFi password: " PSK; echo; fi
case "$PSK" in *$'\n'*|"") echo "Bad WiFi password"; exit 1;; esac
[ ${#PSK} -ge 8 ] && [ ${#PSK} -le 63 ] || { echo "WPA2 passwords are 8-63 characters"; exit 1; }

# Keep the image's original files once, for reference (cloud-init ignores *.orig)
for f in user-data meta-data network-config; do
  [ -f "$BOOT/$f.orig" ] || cp "$BOOT/$f" "$BOOT/$f.orig"
done

INSTANCE="csi-$(date +%Y%m%d-%H%M%S)"
cat > "$BOOT/meta-data" <<EOF
# Written by prepare_sd_card.sh. A NEW instance id makes cloud-init apply
# user-data again on the next boot (it compares with its cached copy).
dsmode: local
instance-id: $INSTANCE
instance_id: $INSTANCE
EOF

cat > "$BOOT/user-data" <<EOF
#cloud-config
# Written by prepare_sd_card.sh for the CSI sensing setup.
# Applied once, on the first boot with this instance id (see meta-data).

users:
  - name: $PI_USER
    gecos: CSI sensing
    groups: [adm, dialout, cdrom, audio, users, sudo, video, games, plugdev, input, gpio, spi, i2c, netdev, render, lpadmin]
    shell: /bin/bash
    sudo: "ALL=(ALL) NOPASSWD:ALL"
    lock_passwd: true
    ssh_authorized_keys:
${KEY_LINES}
write_files:
  - path: /etc/NetworkManager/system-connections/csi-linksys222.nmconnection
    owner: root:root
    permissions: '0600'
    content: |
      [connection]
      id=csi-linksys222
      type=wifi
      interface-name=wlan0
      autoconnect=true
      autoconnect-priority=50

      [wifi]
      mode=infrastructure
      ssid=$SSID
      powersave=2

      [wifi-security]
      key-mgmt=wpa-psk
      psk=$PSK

      [ipv4]
      method=manual
      address1=$PI_IP/$PREFIX
      never-default=true
      may-fail=false

      [ipv6]
      method=disabled

runcmd:
  - [raspi-config, nonint, do_wifi_country, $COUNTRY]
  - [rfkill, unblock, wifi]
  - [nmcli, radio, wifi, "on"]
  - [nmcli, connection, reload]
  - [systemctl, enable, --now, ssh]
  - [sh, -c, "nmcli --wait 30 connection up csi-linksys222 || true"]
  # The password has been used: wipe it from the (world-readable) boot partition
  - [sed, -i, "s/^\\\\(\\\\s*psk=\\\\).*/\\\\1REMOVED-AFTER-FIRST-BOOT/", /boot/firmware/user-data]
EOF
PSK=""

: > "$BOOT/ssh"      # sshswitch.service: also turns SSH on at boot
sync
echo "Boot partition prepared: user '$PI_USER', SSH on, $SSID at $PI_IP (instance $INSTANCE)."
echo "Unmount the card, put it in the Pi, keep the ethernet cable plugged in, power on."

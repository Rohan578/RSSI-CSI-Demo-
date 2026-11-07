#!/usr/bin/env bash
# ============================================================================
#  use_home_wifi.sh  -  put the Pi on an ordinary home WiFi (e.g. ORBI21)
#  instead of the isolated linksys222 network. Run on the LAPTOP:
#
#     ./deploy/use_home_wifi.sh            # ORBI21
#     ./deploy/use_home_wifi.sh MyWiFi     # another network
#
#  - The password comes from this laptop's saved connection for that network
#    (or you type it) and is piped over SSH straight into a root-only file on
#    the Pi. It is never shown, never on a command line, never in git.
#  - The Pi uses the network's own DHCP (router) and is reachable as
#    csi-pi.local. Its linksys222 auto-join is turned off.
#  - The Pi's own DHCP server is NEVER started here (setup_pi.sh only allows
#    it on linksys222).
#  Pi address: PI=csi-pi-eth (cable, default) or PI=csi-pi (WiFi) - see ~/.ssh/config.
#  REJECT_DHCP="172.27.35.1": ignore a second (bad) DHCP server on that network
#  (ORBI21 has one - see the README, "Network design").
# ============================================================================
set -euo pipefail
SSID="${1:-ORBI21}"
PI="${PI:-csi-pi-eth}"
CON="csi-$(printf '%s' "$SSID" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9' '-')"
REJECT_DHCP="${REJECT_DHCP:-}"

PSK=""
if command -v nmcli >/dev/null; then
  while IFS=: read -r name type; do
    [ "$type" = "802-11-wireless" ] || continue
    if [ "$(nmcli -g 802-11-wireless.ssid connection show "$name" 2>/dev/null)" = "$SSID" ]; then
      PSK=$(nmcli -s -g 802-11-wireless-security.psk connection show "$name" 2>/dev/null || true)
      [ -n "$PSK" ] && break
    fi
  done < <(nmcli -t -f NAME,TYPE connection show)
fi
[ -n "$PSK" ] || { read -rsp "$SSID WiFi password: " PSK; echo; }

# The script that runs ON THE PI. Its first input line is the password.
read -r -d '' REMOTE_SCRIPT <<'PI_SIDE' || true
IFS= read -r PSK
[ ${#PSK} -ge 8 ] || { echo "no password received"; exit 1; }
F="/etc/NetworkManager/system-connections/$CON.nmconnection"
sudo install -m 600 -o root -g root /dev/null "$F"
printf '%s\n' \
  "[connection]" "id=$CON" "type=wifi" "interface-name=wlan0" \
  "autoconnect=true" "autoconnect-priority=40" "" \
  "[wifi]" "mode=infrastructure" "ssid=$SSID" "powersave=2" "" \
  "[wifi-security]" "key-mgmt=wpa-psk" "psk=$PSK" "" \
  "[ipv4]" "method=auto" "" "[ipv6]" "method=auto" | sudo tee "$F" >/dev/null
unset PSK
sudo nmcli connection modify csi-linksys222 connection.autoconnect no 2>/dev/null || true
sudo systemctl stop csi-dhcp 2>/dev/null || true
sudo nmcli connection reload
[ -n "$REJECT_DHCP" ] && sudo nmcli connection modify "$CON" ipv4.dhcp-reject-servers "$REJECT_DHCP"
sudo nmcli --wait 45 connection up "$CON" | tail -1
echo "Pi on $SSID: $(ip -br -4 addr show wlan0 | awk '{print $3}')  (name: $(hostname).local)"
PI_SIDE

{ printf '%s\n' "$PSK"; } | ssh -o BatchMode=yes "$PI" \
  "export SSID=$(printf '%q' "$SSID") CON=$(printf '%q' "$CON") REJECT_DHCP=$(printf '%q' "$REJECT_DHCP"); bash -c $(printf '%q' "$REMOTE_SCRIPT")"
PSK=""

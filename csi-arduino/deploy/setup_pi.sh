#!/usr/bin/env bash
# ============================================================================
#  setup_pi.sh  -  runs ON the Raspberry Pi (as root). Safe to run again.
#  Normally started for you by deploy_to_pi.sh from the laptop.
#
#  1. Installs the Python packages (needs internet - the ethernet link to the
#     laptop provides it; linksys222 has none)
#  2. Sets the WiFi country so the Pi's WiFi is allowed to turn on
#  3. Joins linksys222 with the FIXED address 192.168.1.50 (the S3 sends
#     there). Prefers 5 GHz so the Pi's own traffic stays off the 2.4 GHz
#     sensing channel. Never used as the internet route.
#  4. Checks nobody else is handing out addresses on linksys222
#  5. Starts a small DHCP server on wlan0 ONLY (phones/laptops joining
#     linksys222 get 192.168.1.100-199). Never touches eth0.
#  6. Installs server.py as a service: starts at boot, restarts on crash
#  7. Opens the firewall ports if ufw is on, prints a summary
#
#  Settings can be overridden:  sudo WIFI_BAND=bg COUNTRY=CA bash setup_pi.sh
# ============================================================================
set -euo pipefail

SSID="${SSID:-linksys222}"
PI_IP="${PI_IP:-192.168.1.50}"
PREFIX="${PREFIX:-24}"
DHCP_START="${DHCP_START:-192.168.1.100}"
DHCP_END="${DHCP_END:-192.168.1.199}"
WIFI_BAND="${WIFI_BAND:-a}"          # a = 5 GHz, bg = 2.4 GHz, "" = either
COUNTRY="${COUNTRY:-US}"
WIFI_IF="${WIFI_IF:-wlan0}"
CON="csi-linksys222"

PI_USER="${SUDO_USER:-${PI_USER:-csi}}"
HOME_DIR="$(getent passwd "$PI_USER" | cut -d: -f6)"
CSI_DIR="$HOME_DIR/csi"
PSK_FILE="$CSI_DIR/.wifi_psk"

step() { echo; echo "=== $* ==="; }
warn() { echo "!!! $*"; }
[ "$(id -u)" = 0 ] || { echo "Run with sudo:  sudo bash $0"; exit 1; }
[ -d "$CSI_DIR/pi_server" ] || { echo "$CSI_DIR/pi_server not found - run deploy_to_pi.sh from the laptop"; exit 1; }

# ---------------------------------------------------------------------------
step "1/7 Python packages"
if python3 -c "import flask, numpy, sklearn, joblib" 2>/dev/null && command -v nmap >/dev/null && [ -x /usr/sbin/dnsmasq ]; then
  echo "Already installed."
elif curl -fsS -m 8 -o /dev/null http://deb.debian.org/ 2>/dev/null || ping -c1 -W3 1.1.1.1 >/dev/null 2>&1; then
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-flask python3-numpy python3-sklearn \
      python3-joblib nmap dnsmasq-base iw rfkill
else
  warn "No internet and packages missing. Plug the Pi into the laptop's ethernet and run again."
  exit 1
fi
python3 -c "import flask, numpy, sklearn; print('flask', flask.__version__ if hasattr(flask,'__version__') else '?', '| numpy', numpy.__version__, '| scikit-learn', sklearn.__version__)" 2>/dev/null || true

# ---------------------------------------------------------------------------
step "2/7 WiFi country ($COUNTRY)"
if command -v raspi-config >/dev/null; then
  raspi-config nonint do_wifi_country "$COUNTRY" || warn "raspi-config could not set the country"
else
  iw reg set "$COUNTRY" || true
fi
rfkill unblock wifi || true
nmcli radio wifi on || true
echo "WiFi profiles already on this Pi (left untouched):"
nmcli -t -f NAME,TYPE,AUTOCONNECT connection show | awk -F: '$2 ~ /wireless/ {print "   " $1 " (autoconnect " $3 ")"}' | grep -v "$CON" || echo "   (none)"
echo "Currently on WiFi: $(nmcli -t -f DEVICE,CONNECTION device | awk -F: -v d="$WIFI_IF" '$1==d {print $2}')"

# ---------------------------------------------------------------------------
step "3/7 Join $SSID as $PI_IP"
PSK=""
if [ -f "$PSK_FILE" ]; then PSK="$(cat "$PSK_FILE")"; fi
if [ -z "$PSK" ] && nmcli -t -f NAME connection show | grep -qx "$CON"; then
  # e.g. created at first boot by prepare_sd_card.sh: reuse its password
  PSK="$(nmcli -s -g 802-11-wireless-security.psk connection show "$CON" 2>/dev/null || true)"
  [ -n "$PSK" ] && echo "Keeping the password already saved in $CON."
fi
if [ -z "$PSK" ]; then read -rsp "$SSID WiFi password: " PSK; echo; fi
[ -n "$PSK" ] || { warn "No WiFi password - stopping before changing the WiFi setup."; exit 1; }
nmcli connection delete "$CON" >/dev/null 2>&1 || true
nmcli connection add type wifi ifname "$WIFI_IF" con-name "$CON" ssid "$SSID" \
  802-11-wireless.band "$WIFI_BAND" \
  wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PSK" \
  ipv4.method manual ipv4.addresses "$PI_IP/$PREFIX" ipv4.never-default yes ipv4.may-fail no \
  ipv6.method disabled 802-11-wireless.powersave 2 \
  connection.autoconnect yes connection.autoconnect-priority 50 >/dev/null
PSK=""
[ -f "$PSK_FILE" ] && { shred -u "$PSK_FILE" 2>/dev/null || rm -f "$PSK_FILE"; }

if ! nmcli --wait 40 connection up "$CON"; then
  if [ "$WIFI_BAND" = "a" ]; then
    warn "Could not join on 5 GHz - trying either band"
    nmcli connection modify "$CON" 802-11-wireless.band ""
    nmcli --wait 40 connection up "$CON" || { warn "Could not join $SSID. Wrong password? AP off?"; exit 1; }
  else
    warn "Could not join $SSID. Wrong password? AP off?"; exit 1
  fi
fi
iw dev "$WIFI_IF" set power_save off 2>/dev/null || true
iw dev "$WIFI_IF" link | sed -n 's/^\s*\(SSID\|freq\|signal\|tx bitrate\)/   &/p'

# ---------------------------------------------------------------------------
step "4/7 Is anyone else handing out addresses on $SSID?"
systemctl stop csi-dhcp 2>/dev/null || true
OTHER_DHCP=""
if command -v nmap >/dev/null; then
  OUT="$(timeout 25 nmap --script broadcast-dhcp-discover --script-args broadcast-dhcp-discover.timeout=8 -e "$WIFI_IF" 2>/dev/null || true)"
  if echo "$OUT" | grep -q "IP Offered"; then
    OTHER_DHCP="$(echo "$OUT" | grep -E "Server Identifier|IP Offered|Subnet Mask|Router" | sed 's/^/   /')"
  fi
fi
if [ -n "$OTHER_DHCP" ]; then
  warn "Another DHCP server answered on $SSID:"
  echo "$OTHER_DHCP"
  warn "NOT starting the Pi's DHCP server (two would fight). The fixed addresses still work"
  warn "if that network is 192.168.1.x - otherwise change PI_IP here and in the S3's config.h."
else
  echo "Nobody answered - the Pi will hand out addresses."
fi

# ---------------------------------------------------------------------------
step "5/7 DHCP server on $WIFI_IF only"
if [ -z "$OTHER_DHCP" ]; then
  if systemctl is-active --quiet dnsmasq 2>/dev/null; then
    warn "A system-wide dnsmasq service is running; it may clash. Check: systemctl status dnsmasq"
  fi
  mkdir -p /etc/csi
  cat > /etc/csi/dnsmasq-linksys222.conf <<EOF
# DHCP for the isolated linksys222 sensing network. Written by setup_pi.sh.
# Only on $WIFI_IF, no DNS, no router (there's no internet on this network).
port=0
interface=$WIFI_IF
bind-dynamic
no-dhcp-interface=eth0
dhcp-range=$DHCP_START,$DHCP_END,255.255.255.0,12h
dhcp-option=3
dhcp-option=6
dhcp-leasefile=/var/lib/misc/csi-dnsmasq.leases
log-dhcp
EOF
  cat > /etc/systemd/system/csi-dhcp.service <<EOF
[Unit]
Description=DHCP for linksys222 (CSI sensing network, $WIFI_IF only)
After=NetworkManager.service network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/sbin/dnsmasq --keep-in-foreground --conf-file=/etc/csi/dnsmasq-linksys222.conf
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable --now csi-dhcp
  sleep 1
  systemctl is-active --quiet csi-dhcp && echo "DHCP server running." || warn "DHCP server failed: journalctl -u csi-dhcp"
else
  systemctl disable --now csi-dhcp 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
step "6/7 CSI server service"
mkdir -p "$CSI_DIR/pi_server/data" "$CSI_DIR/pi_server/models"
chown -R "$PI_USER": "$CSI_DIR"
cat > /etc/systemd/system/csi-server.service <<EOF
[Unit]
Description=CSI sensing server (UDP 5005 + dashboard on 5000)
After=network-online.target
Wants=network-online.target

[Service]
User=$PI_USER
WorkingDirectory=$CSI_DIR/pi_server
ExecStart=/usr/bin/python3 -u server.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable csi-server >/dev/null
systemctl restart csi-server
systemctl enable --now ssh >/dev/null 2>&1 || true
sleep 3
systemctl is-active --quiet csi-server && echo "csi-server running." || warn "csi-server failed: journalctl -u csi-server -n 50"

# ---------------------------------------------------------------------------
step "7/7 Firewall + summary"
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
  ufw allow 22/tcp; ufw allow 5000/tcp; ufw allow 5005/udp; ufw allow in on "$WIFI_IF" to any port 67 proto udp
fi
ping -c1 -W2 192.168.1.10 >/dev/null 2>&1 && echo "Aruba AP (192.168.1.10) answers from linksys222." \
  || echo "Aruba AP (192.168.1.10) doesn't answer pings from linksys222 (fine for sensing; see AP_SETUP.md)."
echo
echo "Pi addresses:  $(hostname -I)"
echo "Dashboard:     http://$PI_IP:5000/        3D view: http://$PI_IP:5000/room"
echo "Volunteers:    http://$PI_IP:5000/collect"
echo "S3 receiver should appear as 192.168.1.60 sending to $PI_IP:5005."
echo "Logs:          journalctl -u csi-server -f"
echo "Clock:         $(date)  (no internet on linksys222 = no time sync; fine while ethernet is plugged in)"

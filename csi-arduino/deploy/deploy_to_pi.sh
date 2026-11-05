#!/usr/bin/env bash
# ============================================================================
#  deploy_to_pi.sh  -  run on the LAPTOP while the Pi is on the ethernet cable
#
#     ./deploy/deploy_to_pi.sh            first time: key + copy + setup
#     ./deploy/deploy_to_pi.sh --code     later: just copy the code + restart
#
#  1. Installs your SSH key on the Pi (asks the Pi password once)
#  2. Copies pi_server/ and deploy/ to ~/csi on the Pi
#     (never overwrites the Pi's recorded data/ or trained models/)
#  3. Hands the Pi the linksys222 WiFi password (from this laptop's saved
#     WiFi, or you type it) and runs setup_pi.sh there with sudo
#  4. Adds "csi-pi" (over linksys222) and "csi-pi-eth" (over the cable)
#     to ~/.ssh/config, so afterwards:   ssh csi-pi
#
#  Needs: SSH enabled on the Pi (sudo systemctl enable --now ssh, or an empty
#  file named "ssh" on the SD card's boot partition).
# ============================================================================
set -euo pipefail
PI_USER="${PI_USER:-basbhat187}"
PI_ETH="${PI_ETH:-10.42.0.222}"          # Pi on the laptop's ethernet link
PI_WIFI="${PI_WIFI:-192.168.1.50}"       # Pi on linksys222 (set by setup_pi.sh)
KEY="${KEY:-$HOME/.ssh/id_ed25519}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="$PI_USER@$PI_ETH"
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 -i "$KEY")

echo "== Pi at $TARGET"
if ! timeout 5 bash -c "</dev/tcp/$PI_ETH/22" 2>/dev/null; then
  echo "SSH (port 22) is not open on $PI_ETH."
  echo "On the Pi:  sudo systemctl enable --now ssh   (keyboard + screen), or put an empty"
  echo "file named 'ssh' on the SD card's boot partition and reboot it."
  exit 1
fi

if [ "${1:-}" != "--code" ]; then
  echo "== 1. SSH key ($KEY.pub)"
  if ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$TARGET" true 2>/dev/null; then
    echo "Key already works."
  else
    ssh-copy-id -o StrictHostKeyChecking=accept-new -i "$KEY.pub" "$TARGET"
  fi
fi

echo "== 2. Copying code to ~/csi"
tar -C "$ROOT" --exclude='pi_server/data' --exclude='pi_server/models/*.joblib' \
    --exclude='pi_server/models/*.json' --exclude='__pycache__' --exclude='*.pyc' \
    -czf - pi_server deploy | ssh "${SSH_OPTS[@]}" "$TARGET" 'mkdir -p ~/csi && tar -C ~/csi -xzf -'

if [ "${1:-}" = "--code" ]; then
  ssh "${SSH_OPTS[@]}" -t "$TARGET" 'sudo systemctl restart csi-server && systemctl --no-pager status csi-server | head -5'
  exit 0
fi

echo "== 3. WiFi password for the Pi"
PSK=""
if command -v nmcli >/dev/null; then
  PSK=$(nmcli -s -g 802-11-wireless-security.psk connection show linksys222 2>/dev/null || true)
fi
if [ -z "$PSK" ]; then read -rsp "linksys222 WiFi password: " PSK; echo; fi
printf '%s' "$PSK" | ssh "${SSH_OPTS[@]}" "$TARGET" 'umask 077; cat > ~/csi/.wifi_psk'
PSK=""

echo "== 4. Running setup on the Pi (may ask for the Pi password for sudo)"
ssh "${SSH_OPTS[@]}" -t "$TARGET" 'sudo bash ~/csi/deploy/setup_pi.sh'

echo "== 5. SSH shortcuts in ~/.ssh/config"
touch ~/.ssh/config
if ! grep -q "^Host csi-pi$" ~/.ssh/config; then
  cat >> ~/.ssh/config <<EOF

# Raspberry Pi 5 for CSI sensing (added by deploy_to_pi.sh)
Host csi-pi
    HostName $PI_WIFI
    User $PI_USER
    IdentityFile $KEY
Host csi-pi-eth
    HostName $PI_ETH
    User $PI_USER
    IdentityFile $KEY
EOF
  echo "Added: ssh csi-pi (via linksys222)   ssh csi-pi-eth (via the cable)"
else
  echo "Already there."
fi
echo
echo "Done. Dashboard now (over the cable): http://$PI_ETH:5000/room"

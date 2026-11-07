#!/usr/bin/env bash
# ============================================================================
#  Put the WiFi password into the receiver sketches (S3 + C5) just before
#  uploading. Run on the laptop:   ./deploy/fill_wifi_password.sh
#
#  The network is whatever WIFI_SSID says in csi_receiver_s3/config.h
#  (currently ORBI21). The password comes from this laptop's saved WiFi
#  connection for that network, or you type it (--prompt). It is written
#  ONLY into the two local config.h files and never printed.
#
#  !!! After this, config.h contains the password: don't commit/push it. !!!
#  Put the placeholder back afterwards:   ./deploy/fill_wifi_password.sh --clear
# ============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILES=("$ROOT/csi_receiver_s3/config.h" "$ROOT/csi_receiver_c5/config.h")
PLACEHOLDER="__WIFI_PASSWORD__"

if [ "${1:-}" = "--clear" ]; then
  for f in "${FILES[@]}"; do
    sed -i -E "s|^(#define WIFI_PASSWORD +)\".*\"|\1\"$PLACEHOLDER\"|" "$f"
  done
  echo "Placeholder restored in both config.h files."
  exit 0
fi

SSID="$(sed -n -E 's/^#define WIFI_SSID +"(.*)".*/\1/p' "$ROOT/csi_receiver_s3/config.h")"
[ -n "$SSID" ] || { echo "WIFI_SSID not found in csi_receiver_s3/config.h"; exit 1; }
echo "Network (from config.h): $SSID"

PSK=""
if [ "${1:-}" != "--prompt" ] && command -v nmcli >/dev/null 2>&1; then
  # Find this laptop's saved connection for that network name
  while IFS=: read -r name type; do
    [ "$type" = "802-11-wireless" ] || continue
    if [ "$(nmcli -g 802-11-wireless.ssid connection show "$name" 2>/dev/null)" = "$SSID" ]; then
      PSK=$(nmcli -s -g 802-11-wireless-security.psk connection show "$name" 2>/dev/null || true)
      [ -n "$PSK" ] && { echo "Using the password saved on this laptop for \"$SSID\"."; break; }
    fi
  done < <(nmcli -t -f NAME,TYPE connection show)
fi
if [ -z "$PSK" ]; then
  read -rsp "$SSID WiFi password: " PSK; echo
fi

PSK="$PSK" python3 - "${FILES[@]}" <<'PYEOF'
import os, re, sys
psk = os.environ["PSK"]
if not 8 <= len(psk) <= 63:
    sys.exit("That doesn't look like a WPA2 password (8-63 characters).")
c_string = psk.replace("\\", "\\\\").replace('"', '\\"')
for path in sys.argv[1:]:
    text = open(path).read()
    new, n = re.subn(r'^(#define WIFI_PASSWORD +)".*"', lambda m: m.group(1) + '"' + c_string + '"',
                     text, count=1, flags=re.M)
    if n != 1:
        sys.exit(f"WIFI_PASSWORD line not found in {path}")
    open(path, "w").write(new)
    print("  updated", os.path.relpath(path))
PYEOF
echo "Done. Upload csi_receiver_s3 to the ESP32-S3, then run:  $0 --clear"

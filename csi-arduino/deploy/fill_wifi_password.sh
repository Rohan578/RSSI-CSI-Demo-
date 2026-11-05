#!/usr/bin/env bash
# ============================================================================
#  Put the linksys222 WiFi password into the receiver sketches (S3 + C5).
#  Run on the laptop, from anywhere:   ./deploy/fill_wifi_password.sh
#  It uses the password this laptop already has saved for linksys222, or
#  asks you to type it (also:  ./deploy/fill_wifi_password.sh --prompt).
#
#  After this, config.h contains the password: don't commit/push it.
#  To put the placeholder back:   ./deploy/fill_wifi_password.sh --clear
# ============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILES=("$ROOT/csi_receiver_s3/config.h" "$ROOT/csi_receiver_c5/config.h")
PLACEHOLDER="__LINKSYS222_PSK__"

if [ "${1:-}" = "--clear" ]; then
  for f in "${FILES[@]}"; do
    sed -i -E "s|^(#define WIFI_PASSWORD +)\".*\"|\1\"$PLACEHOLDER\"|" "$f"
  done
  echo "Placeholder restored in both config.h files."
  exit 0
fi

PSK=""
if [ "${1:-}" != "--prompt" ] && command -v nmcli >/dev/null 2>&1; then
  PSK=$(nmcli -s -g 802-11-wireless-security.psk connection show linksys222 2>/dev/null || true)
  [ -n "$PSK" ] && echo "Using the linksys222 password saved on this laptop."
fi
if [ -z "$PSK" ]; then
  read -rsp "linksys222 WiFi password: " PSK; echo
fi

PSK="$PSK" python3 - "${FILES[@]}" <<'EOF'
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
EOF
echo "Done. Re-upload csi_receiver_s3 to the ESP32-S3. (Don't push config.h with the password in it.)"

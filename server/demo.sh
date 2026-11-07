#!/usr/bin/env bash
# ============================================================================
#  demo.sh  -  try the whole server side WITHOUT any ESP32 hardware
#             (bash; on Windows use:  python run_demo.py)
#
#     ./demo.sh                 server + simulated basement (0,1,2,3... people)
#     ./demo.sh --people 2      always 2 simulated people (no auto-calibration)
#
#  Then open  http://localhost:5000/room   (3D view)
#             http://localhost:5000/collect  (volunteer page; try --follow-collect)
#  Ctrl+C stops everything. Demo data goes to /tmp/csi-demo, not data/.
#  Needs: python3 with flask + numpy (pip install -r requirements.txt)
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"
PY="${PYTHON:-python3}"
DEMO="${CSI_DEMO_DIR:-/tmp/csi-demo}"
mkdir -p "$DEMO/data" "$DEMO/models"
export CSI_DATA_DIR="$DEMO/data"
export CSI_MODELS_DIR="${CSI_MODELS_DIR:-$DEMO/models}"
"$PY" -c "import flask, numpy" 2>/dev/null || { echo "Needs flask + numpy:  pip install -r requirements.txt"; exit 1; }

SRV="" SIM=""
trap 'kill $SRV $SIM 2>/dev/null || true' EXIT INT TERM
"$PY" server.py & SRV=$!
sleep 2
"$PY" sim_room.py "$@" & SIM=$!
sleep 4
if [ $# -eq 0 ]; then
  # The default script starts with 25 s of empty room: use it as the calibration
  echo "[DEMO] Calibrating on the simulated empty room (18 s)..."
  curl -s -X POST -H 'Content-Type: application/json' -d '{"seconds":18}' \
       http://127.0.0.1:5000/api/calibrate >/dev/null || true
fi
echo "[DEMO] Open http://localhost:5000/room   (Ctrl+C to stop)"
wait "$SRV"

"""
============================================================================
 run_demo.py  -  try everything WITHOUT any ESP32 hardware
                 (Windows, macOS and Linux - the same as demo.sh, in Python)
============================================================================
   python run_demo.py                  server + simulated room (10 TX, S3, C5)
   python run_demo.py --interference   ...with home-WiFi interference bursts
   python run_demo.py --people 2       always 2 simulated people

 Then open  http://localhost:5000/room     (3D view)
            http://localhost:5000/collect  (volunteer page)
 Ctrl+C stops everything. Demo data goes to a temporary folder, not data/.
 Needs: pip install -r requirements.txt
============================================================================
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    try:
        import flask, numpy  # noqa: F401
    except ImportError:
        sys.exit("Needs flask + numpy:  pip install -r requirements.txt")

    demo = os.environ.get("CSI_DEMO_DIR") or tempfile.mkdtemp(prefix="csi-demo-")
    env = dict(os.environ, CSI_DATA_DIR=os.path.join(demo, "data"),
               CSI_MODELS_DIR=os.environ.get("CSI_MODELS_DIR", os.path.join(demo, "models")))
    os.makedirs(env["CSI_DATA_DIR"], exist_ok=True)
    os.makedirs(env["CSI_MODELS_DIR"], exist_ok=True)

    sim_args = sys.argv[1:]
    server = subprocess.Popen([sys.executable, os.path.join(HERE, "server.py")], env=env, cwd=HERE)
    sim = None
    try:
        time.sleep(2.5)
        sim = subprocess.Popen([sys.executable, os.path.join(HERE, "sim_room.py")] + sim_args, cwd=HERE)
        time.sleep(4)
        if "--people" not in sim_args and "--follow-collect" not in sim_args:
            # The default script starts with 25 s of empty room: use it as the calibration
            print("[DEMO] Calibrating on the simulated empty room (18 s)...")
            req = urllib.request.Request("http://127.0.0.1:5000/api/calibrate",
                                         data=json.dumps({"seconds": 18}).encode(),
                                         headers={"Content-Type": "application/json"})
            try:
                urllib.request.urlopen(req, timeout=3).read()
            except OSError as error:
                print(f"[DEMO] Could not start calibration: {error}")
        print(f"[DEMO] Open http://localhost:5000/room   (Ctrl+C to stop; data in {demo})")
        server.wait()
    except KeyboardInterrupt:
        pass
    finally:
        for p in (sim, server):
            if p and p.poll() is None:
                p.terminate()


if __name__ == "__main__":
    main()

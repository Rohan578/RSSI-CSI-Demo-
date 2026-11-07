"""
============================================================================
 CSI Collector + Live Dashboard  (runs on the Raspberry Pi 5)
============================================================================

 SYSTEM
   Transmitters (ESP32 / C6 / C3) --ESP-NOW--> ESP32-S3 receiver (2.4 GHz) --UDP--> this Pi
   Access point  --ping replies-->  ESP32-C5 receiver (5 GHz)  ----------UDP--> this Pi
   Every CSI sample describes one LINK: the radio path from one sender to
   one receiver, on one band. Links are named like "node 3 -> rx 1 (2.4 GHz)".
   "node 0" always means the access point.

 WHAT THIS DOES
   1. UDP listener (background thread)
        Receives CSI batches from every receiver (S3, C5), decodes them
        (format in SECTION 2), and keeps live stats per link.
   2. CSV logger
        Saves every CSI sample to data/csi_<date>_<time>.csv together with
        the CURRENT LABEL (e.g. "empty", "1 person", "walking"). Labeled
        data is what you train the AI model on.
   3. Flask web dashboard
        http://<pi-ip>:5000/            live view: rate, RSSI, motion, CSI shape
        http://<pi-ip>:5000/api/links   the same numbers as JSON
        http://<pi-ip>:5000/api/label   GET/POST the current label

 SETUP
   pip install flask
   python3 server.py

 TEST WITHOUT HARDWARE
   In a second terminal:  python3 fake_receiver.py

 NOTE ON "MOTION"
   The motion score is a simple baseline (how much the CSI amplitudes are
   wobbling over the last second). It is NOT the AI model - it's a quick
   way to see that sensing works. Replace it with your trained model later
   (see estimate_room_state() in SECTION 4).

 ADDED FOR THE 3D ROOM VIEW (see README_PI.md)
   http://<pi-ip>:5000/room      3D view of the room + people (slow spin)
   http://<pi-ip>:5000/collect   guided data collection for volunteers
   /api/state                    everything the 3D view shows, as JSON
   /api/calibrate (POST)         record the EMPTY room as the reference
   /api/collect/start|stop       run a guided collection session
   /api/model/reload (POST)      reload room.json + the trained AI model
   analysis.py   live scoring + localization (thread)
   localizer.py  room geometry, link-geometry estimate, tracker, AI model
   guided.py     the volunteer schedule + automatic CSV labels
   train_model.py  learns zones from guided sessions (run it ON the Pi)
   sim_room.py   simulated room with walking people (test without hardware)
============================================================================
"""

import csv
import json
import logging
import math
import os
import shutil
import socket
import struct
import threading
import time
from collections import deque
from datetime import datetime

from flask import Flask, jsonify, request, send_from_directory

import csi_features
from analysis import Analyzer
from guided import GuidedSession


# ============================================================================
#  SECTION 1: SETTINGS  -  Change these to match your setup
# ============================================================================

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

UDP_HOST = "0.0.0.0"         # Listen on every network interface
UDP_PORT = 5005              # Must match PI_SERVER_PORT in the S3's config.h

WEB_HOST = "0.0.0.0"         # "0.0.0.0" lets teammates open the dashboard
WEB_PORT = 5000

SAVE_TO_CSV = True           # Write CSI samples to a CSV file (see CSV_ONLY_LABELED)
CSV_ONLY_LABELED = True      # True: only while a guided session runs or a label is set.
                             # (Everything = ~1.4 GB per hour - fills an SD card in a day.)
CSV_MIN_FREE_GB = 2.0        # Stop writing when the disk gets this full
# CSV files, calibration, sessions. (CSI_DATA_DIR / CSI_MODELS_DIR environment
# variables override these - demo.sh uses that to keep demo data out of the way.)
DATA_DIR = os.environ.get("CSI_DATA_DIR", os.path.join(BASE_DIR, "data"))
ROOM_FILE = os.path.join(BASE_DIR, "room.json")  # Room size + where each board is
MODELS_DIR = os.environ.get("CSI_MODELS_DIR", os.path.join(BASE_DIR, "models"))  # train_model.py output
DEFAULT_LABEL = "unlabeled"  # Label used until you set one on the dashboard

MOTION_WINDOW = 50           # Samples used for the motion score (~1 s at 50 Hz)
RATE_WINDOW_S = 2.0          # Seconds used to measure samples per second
BAND_MOTION_THRESHOLD = 5.0  # Motion score that counts as "movement" for the band check
LINK_TIMEOUT_S = 5.0         # Link shown as "offline" after this long silent

PRINT_EVERY_BATCH = False    # True = print a line per UDP packet (noisy)


# ============================================================================
#  SECTION 2: PACKET FORMAT
#  MUST MATCH csi_receiver_s3/csi_packet.h EXACTLY.
#    "<"  little-endian   H=uint16  B=uint8  b=int8  I=uint32  x=pad byte
# ============================================================================

CSI_PACKET_MAGIC = 0xC51A
CSI_PACKET_VERSION = 2

#   magic version receiver_id batch_seq sample_count reserved[3]
BATCH_HEADER = struct.Struct("<HBBIB3x")     # 12 bytes
#   tx_node_id rssi noise channel rx_format first_invalid csi_len tx_seq rx_timestamp_us
SAMPLE_HEADER = struct.Struct("<BbbBBBHII")  # 16 bytes


def parse_batch(data: bytes):
    """
    Decode one UDP packet into (receiver_id, batch_seq, [samples]).
    Each sample is a dict. Returns None if the packet isn't a valid batch.

    >>> EDIT THIS if you add fields to csi_packet.h
    """
    if len(data) < BATCH_HEADER.size:
        return None
    magic, version, receiver_id, batch_seq, count = BATCH_HEADER.unpack_from(data, 0)
    if magic != CSI_PACKET_MAGIC or version != CSI_PACKET_VERSION:
        return None

    samples = []
    offset = BATCH_HEADER.size
    for _ in range(count):
        if offset + SAMPLE_HEADER.size > len(data):
            return None                       # Truncated packet
        (tx_node_id, rssi, noise, channel, rx_format, first_invalid,
         csi_len, tx_seq, rx_ts_us) = SAMPLE_HEADER.unpack_from(data, offset)
        offset += SAMPLE_HEADER.size
        if offset + csi_len > len(data):
            return None
        raw = data[offset:offset + csi_len]
        offset += csi_len

        samples.append({
            "tx_node_id": tx_node_id,
            "tx_seq": tx_seq,               # 0 = unknown
            "rx_timestamp_us": rx_ts_us,
            "rssi": rssi,
            "noise_floor": noise,
            "channel": channel,
            "rx_format": rx_format,
            "first_word_invalid": first_invalid,
            # Signed 8-bit values: [imag, real, imag, real, ...]
            "csi": list(struct.unpack(f"<{csi_len}b", raw)),
            "raw": raw,                     # same bytes, for the numpy feature code
        })
    return receiver_id, batch_seq, samples


# ============================================================================
#  SECTION 3: CSI MATH
# ============================================================================

def band_of(channel):
    """WiFi channels 1-14 are 2.4 GHz; 32 and up are 5 GHz."""
    return "5 GHz" if channel >= 32 else "2.4 GHz"


def sender_name(tx_node_id):
    """Node 0 is the access point; everything else is a transmitter board."""
    return "access point" if tx_node_id == 0 else f"node {tx_node_id}"


def csi_amplitudes(csi_raw):
    """
    Turn raw CSI [imag, real, imag, real, ...] into one amplitude per
    subcarrier: sqrt(real^2 + imag^2). Unused subcarriers come out as 0.
    """
    return [math.hypot(csi_raw[i + 1], csi_raw[i]) for i in range(0, len(csi_raw) - 1, 2)]


def motion_score(amplitude_history):
    """
    Simple movement indicator: for each subcarrier, how much its amplitude
    varies over the window, relative to its average. Averaged over all
    active subcarriers and scaled to roughly 0-100.

    Empty, still room -> low.  People moving -> higher.
    The exact numbers depend on the room; compare against an empty-room
    reading rather than using a fixed threshold.
    """
    if len(amplitude_history) < 10:
        return None
    # Only compare samples with the same length as the newest one
    # (different packet types give different numbers of subcarriers)
    n = len(amplitude_history[-1])
    rows = [a for a in amplitude_history if len(a) == n]
    if len(rows) < 10:
        return None

    ratios = []
    for k in range(n):
        column = [r[k] for r in rows]
        mean = sum(column) / len(column)
        if mean < 1.0:                    # Skip empty (guard) subcarriers
            continue
        var = sum((v - mean) ** 2 for v in column) / len(column)
        ratios.append(math.sqrt(var) / mean)
    if not ratios:
        return None
    return round(100.0 * sum(ratios) / len(ratios), 2)


# ============================================================================
#  SECTION 4: LIVE STATE PER LINK
#  A "link" is one radio path: transmitter node -> receiver.
#  Each link is tracked separately because each crosses a different part
#  of the room.
# ============================================================================

class LinkState:
    """Everything we track about one link (sender -> receiver, on one band)."""

    def __init__(self, receiver_id, tx_node_id, band, receiver_ip):
        self.receiver_id = receiver_id
        self.tx_node_id = tx_node_id
        self.band = band
        self.receiver_ip = receiver_ip
        self.last_seen = 0.0
        self.total_samples = 0
        self.lost_samples = 0          # Gaps in the transmitter's counter
        self.last_tx_seq = None
        self.last_rssi = None
        self.last_channel = None
        self.arrival_times = deque()   # For samples-per-second
        self.amplitudes = deque(maxlen=MOTION_WINDOW)
        # Last 60 s of normalized amplitudes, for analysis.py (3D view / AI)
        self.buf = csi_features.LinkBuffer(capacity=3000)
        self.layout_ratios = []        # CSI byte-layout self-check (first 200 samples)
        self.layout_ok = None          # None = still checking

    def add_sample(self, sample, now):
        seq = sample["tx_seq"]
        if seq:                                    # 0 means "counter unknown"
            if self.last_tx_seq is not None:
                if seq > self.last_tx_seq + 1:
                    self.lost_samples += seq - self.last_tx_seq - 1
                elif seq <= self.last_tx_seq:      # Transmitter rebooted
                    self.lost_samples = 0
            self.last_tx_seq = seq
        self.total_samples += 1
        self.last_seen = now
        self.last_rssi = sample["rssi"]
        self.last_channel = sample["channel"]
        self.arrival_times.append(now)
        self.amplitudes.append(csi_amplitudes(sample["csi"]))
        if len(self.layout_ratios) < 200:
            r = csi_features.layout_ratio(sample["raw"])
            if r is not None:
                self.layout_ratios.append(r)
                if len(self.layout_ratios) in (50, 200):
                    ratio = sorted(self.layout_ratios)[len(self.layout_ratios) // 2]
                    self.layout_ok = ratio <= csi_features.LAYOUT_MAX_RATIO
                    if not self.layout_ok:
                        print(f"[CSI] !!! {sender_name(self.tx_node_id)} -> rx {self.receiver_id}: CSI layout "
                              f"doesn't match what csi_features.py expects (guard/data = {ratio:.2f}). "
                              "Positions and the AI model will be wrong for this link.")
        amp = csi_features.normalize(
            csi_features.lltf_amplitudes(sample["raw"], sample["first_word_invalid"]))
        if amp is not None:
            self.buf.add(now, amp, sample["rssi"])

    def summary(self, now):
        while self.arrival_times and now - self.arrival_times[0] > RATE_WINDOW_S:
            self.arrival_times.popleft()
        history = list(self.amplitudes)
        return {
            "link": f"{sender_name(self.tx_node_id)} -> rx {self.receiver_id} ({self.band})",
            "sender": sender_name(self.tx_node_id),
            "tx_node_id": self.tx_node_id,
            "band": self.band,
            "receiver_id": self.receiver_id,
            "receiver_ip": self.receiver_ip,
            "online": (now - self.last_seen) < LINK_TIMEOUT_S,
            "seconds_since_seen": round(now - self.last_seen, 1),
            "samples_per_sec": round(len(self.arrival_times) / RATE_WINDOW_S, 1),
            "total_samples": self.total_samples,
            "lost_samples": self.lost_samples,
            "rssi": self.last_rssi,
            "channel": self.last_channel,
            "motion": motion_score(history),
            "latest_amplitudes": [round(a, 1) for a in history[-1]] if history else [],
        }


def estimate_room_state(link_summaries):
    """
    >>> REPLACE THIS with your AI model later.
    For now: the room is "active" if any online link's motion score is
    above MOTION_THRESHOLD. Tune the threshold after watching the scores
    with the room empty vs. with people moving.
    """
    MOTION_THRESHOLD = 5.0
    scores = [l["motion"] for l in link_summaries if l["online"] and l["motion"] is not None]
    if not scores:
        return "no data"
    return "active" if max(scores) > MOTION_THRESHOLD else "still"


def compare_bands(link_summaries):
    """
    Side-by-side numbers for 2.4 GHz vs 5 GHz, from the online links on each
    band. Shown at the top of the dashboard. Useful for seeing which band
    reacts more to the same movement (higher motion score) and which has the
    stronger, cleaner signal.
    """
    result = {}
    for band in ("2.4 GHz", "5 GHz"):
        on_band = [l for l in link_summaries if l["band"] == band and l["online"]]
        motions = [l["motion"] for l in on_band if l["motion"] is not None]
        rssis = [l["rssi"] for l in on_band if l["rssi"] is not None]
        result[band] = {
            "links": len(on_band),
            "avg_motion": round(sum(motions) / len(motions), 2) if motions else None,
            "avg_rssi": round(sum(rssis) / len(rssis), 1) if rssis else None,
            "samples_per_sec": round(sum(l["samples_per_sec"] for l in on_band), 1),
        }

    # Live cross-check (same rule as compare_bands.py):
    # movement on 2.4 GHz but not on 5 GHz usually means 2.4 GHz interference.
    m24, m5 = result["2.4 GHz"]["avg_motion"], result["5 GHz"]["avg_motion"]
    if m24 is None or m5 is None:
        result["note"] = ""
    elif m24 > BAND_MOTION_THRESHOLD and m5 <= BAND_MOTION_THRESHOLD:
        result["note"] = "2.4 GHz moving, 5 GHz still: possible 2.4 GHz interference"
    elif m5 > BAND_MOTION_THRESHOLD and m24 <= BAND_MOTION_THRESHOLD:
        result["note"] = "Only 5 GHz sees movement: probably a small movement"
    else:
        result["note"] = "Bands agree"
    return result


links = {}                     # (receiver_id, tx_node_id, band) -> LinkState
links_lock = threading.Lock()
current_label = DEFAULT_LABEL
label_lock = threading.Lock()
guided = GuidedSession(DATA_DIR)   # Volunteer schedule (/collect); labels rows while it runs
analyzer = None                    # Started in SECTION 8


# ============================================================================
#  SECTION 5: CSV LOGGER
#  One row per CSI sample. The "csi" column holds the raw signed values
#  separated by spaces: imag real imag real ...
#  To train per link, group rows by (receiver_id, tx_node_id, band).
#  tx_node_id 0 = the access point.
# ============================================================================

CSV_COLUMNS = ["pi_time", "label", "receiver_id", "tx_node_id", "band", "receiver_ip",
               "batch_seq", "tx_seq", "rx_timestamp_us", "rssi", "noise_floor",
               "channel", "rx_format", "first_word_invalid", "csi_len", "csi"]


class CsvLogger:
    def __init__(self, folder):
        os.makedirs(folder, exist_ok=True)
        name = datetime.now().strftime("csi_%Y%m%d_%H%M%S.csv")
        self.path = os.path.join(folder, name)
        self.file = open(self.path, "w", newline="")
        self.writer = csv.writer(self.file)
        self.writer.writerow(CSV_COLUMNS)
        self.rows = 0
        self.disk_full = False
        self.last_flush = time.time()
        print(f"[CSV] Logging to {self.path}")

    def write(self, label, receiver_id, ip, batch_seq, sample, now):
        self.writer.writerow([
            f"{now:.3f}", label, receiver_id, sample["tx_node_id"], band_of(sample["channel"]), ip,
            batch_seq, sample["tx_seq"], sample["rx_timestamp_us"],
            sample["rssi"], sample["noise_floor"], sample["channel"],
            sample["rx_format"], sample["first_word_invalid"],
            len(sample["csi"]), " ".join(map(str, sample["csi"])),
        ])
        self.rows += 1
        if self.rows % 5000 == 0:            # Every ~20 s: is the disk filling up?
            free_gb = shutil.disk_usage(os.path.dirname(self.path)).free / 1e9
            if free_gb < CSV_MIN_FREE_GB and not self.disk_full:
                print(f"[CSV] Only {free_gb:.1f} GB free - STOPPED writing. Delete old data/csi_*.csv")
            self.disk_full = free_gb < CSV_MIN_FREE_GB
        if now - self.last_flush > 1.0:      # Flush to disk about once a second
            self.file.flush()
            self.last_flush = now


csv_logger = None


# ============================================================================
#  SECTION 6: UDP LISTENER
# ============================================================================

def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind((UDP_HOST, UDP_PORT))
    print(f"[UDP] Listening for CSI on port {UDP_PORT}")

    while True:
        try:
            data, (ip, _port) = sock.recvfrom(4096)
            parsed = parse_batch(data)
            if parsed is None:
                print(f"[UDP] Ignored a packet from {ip} that isn't a v{CSI_PACKET_VERSION} CSI batch")
                continue
            receiver_id, batch_seq, samples = parsed
            now = time.time()

            label = guided.label(now)          # A guided session labels rows itself
            if label is None:
                with label_lock:
                    label = current_label
            with links_lock:
                for s in samples:
                    band = band_of(s["channel"])
                    key = (receiver_id, s["tx_node_id"], band)
                    link = links.get(key)
                    if link is None:
                        link = links[key] = LinkState(receiver_id, s["tx_node_id"], band, ip)
                        print(f"[UDP] New link: {sender_name(s['tx_node_id'])} -> receiver "
                              f"{receiver_id} on {band}, channel {s['channel']} ({ip})")
                    link.receiver_ip = ip
                    link.add_sample(s, now)
                    if csv_logger and not csv_logger.disk_full and (
                            not CSV_ONLY_LABELED or label != DEFAULT_LABEL):
                        csv_logger.write(label, receiver_id, ip, batch_seq, s, now)

            if PRINT_EVERY_BATCH:
                print(f"[UDP] receiver {receiver_id} batch {batch_seq}: {len(samples)} samples")
        except Exception as error:
            print(f"[UDP] Error: {error}")


# ============================================================================
#  SECTION 7: FLASK WEB DASHBOARD
# ============================================================================

app = Flask(__name__)


@app.route("/api/links")
def api_links():
    now = time.time()
    with links_lock:
        summaries = [l.summary(now) for _, l in sorted(links.items())]
    with label_lock:
        label = current_label
    return jsonify({
        "room_state": estimate_room_state(summaries),
        "label": label,
        "csv_file": csv_logger.path if csv_logger else None,
        "csv_rows": csv_logger.rows if csv_logger else 0,
        "bands": compare_bands(summaries),
        "links": summaries,
    })


@app.route("/api/label", methods=["GET", "POST"])
def api_label():
    global current_label
    if request.method == "POST":
        body = request.get_json(silent=True) or request.form
        new_label = str(body.get("label", "")).strip() or DEFAULT_LABEL
        with label_lock:
            current_label = new_label
        print(f"[LABEL] Now recording as: {new_label}")
    with label_lock:
        return jsonify({"label": current_label})


# --- 3D room view, guided collection, calibration, AI model -----------------

STATIC_DIR = os.path.join(BASE_DIR, "static")


@app.route("/room")
def room_page():
    return send_from_directory(STATIC_DIR, "room.html")


@app.route("/collect")
def collect_page():
    return send_from_directory(STATIC_DIR, "collect.html")


@app.route("/api/state")
def api_state():
    state = dict(analyzer.snapshot())
    state["collect"] = guided.state()
    state["csv_rows"] = csv_logger.rows if csv_logger else 0
    return jsonify(state)


@app.route("/api/room", methods=["GET"])
def api_room():
    with open(ROOM_FILE) as f:
        cfg = json.load(f)
    cfg["zone_names"] = analyzer.room.zone_names()
    return jsonify(cfg)


@app.route("/api/calibrate", methods=["POST"])
def api_calibrate():
    body = request.get_json(silent=True) or {}
    analyzer.start_calibration(body.get("seconds", 20), source="button")
    return jsonify({"ok": True})


@app.route("/api/collect/start", methods=["POST"])
def api_collect_start():
    body = request.get_json(silent=True) or {}
    session = guided.start(
        analyzer.room.zone_names(),
        volunteers=body.get("volunteers", 1),
        hold_s=body.get("hold_s", 20),
        move_s=body.get("move_s", 6),
        rounds=body.get("rounds", 2),
        activities=body.get("activities", ["stand", "walk"]),
        empty_s=body.get("empty_s", 30),
    )
    print(f"[COLLECT] Guided session {session} started")
    return jsonify({"ok": True, "session": session, "state": guided.state()})


@app.route("/api/collect/stop", methods=["POST"])
def api_collect_stop():
    guided.stop()
    print("[COLLECT] Guided session stopped")
    return jsonify({"ok": True})


@app.route("/api/collect", methods=["GET"])
def api_collect():
    return jsonify(guided.state())


@app.route("/api/model/reload", methods=["POST"])
def api_model_reload():
    ok = analyzer.reload()
    return jsonify({"ok": ok, "status": analyzer.model.status})


@app.route("/api/sim/truth", methods=["POST"])
def api_sim_truth():
    """Only sim_room.py uses this: where its fake people really are."""
    body = request.get_json(silent=True) or {}
    people = [{"id": int(p.get("id", 0)), "x": float(p["x"]), "y": float(p["y"])}
              for p in body.get("people", []) if "x" in p and "y" in p]
    analyzer.set_sim_truth(people)
    return jsonify({"ok": True})


DASHBOARD_HTML = r"""
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CSI Dashboard</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 20px; background: #f5f6f8; color: #222; }
  h1 { margin: 0 0 4px; font-size: 22px; }
  .muted { color: #666; font-size: 14px; }
  .bar { display: flex; flex-wrap: wrap; gap: 12px; align-items: center; margin: 14px 0; }
  .pill { padding: 6px 12px; border-radius: 999px; background: #e7e9ee; font-size: 14px; }
  .pill.active { background: #ffe1d2; color: #8a3410; }
  .pill.still  { background: #d9f2e6; color: #0b5d3b; }
  .pill.band   { padding: 2px 8px; font-size: 12px; background: #e3ecfb; color: #1d4a8f; }
  input, button { font-size: 14px; padding: 6px 10px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 14px; }
  .card { background: white; border-radius: 12px; padding: 14px; border: 1px solid #e2e4e8; }
  .card.offline { opacity: .5; }
  .row { display: flex; justify-content: space-between; font-size: 14px; margin: 3px 0; }
  .meter { height: 8px; background: #eceef2; border-radius: 4px; overflow: hidden; margin: 6px 0 10px; }
  .meter div { height: 100%; background: #e0703a; width: 0; transition: width .3s; }
  canvas { width: 100%; height: 110px; background: #fafbfc; border-radius: 6px; }
</style>
</head>
<body>
  <h1>CSI sensing dashboard</h1>
  <div class="muted" id="csv">Waiting for data...</div>
  <div class="bar">
    <a class="pill" href="/room">3D room view &rarr;</a>
    <a class="pill" href="/collect">Guided data collection &rarr;</a>
    <button onclick="calibrate()">Calibrate empty room (20 s)</button>
    <span class="muted" id="calib"></span>
  </div>

  <div class="bar">
    <span>Room:</span> <span class="pill" id="room">no data</span>
    <span style="margin-left:16px">Recording label:</span>
    <input id="label" placeholder="e.g. empty, 1 person, walking">
    <button onclick="setLabel()">Set label</button>
    <span class="pill" id="current-label">-</span>
  </div>

  <div class="bar" id="bands"></div>

  <div class="grid" id="links"></div>

<script>
async function calibrate() {
  if (!confirm('Everyone must be OUT of the room for the next 20 seconds. Start?')) return;
  await fetch('/api/calibrate', {method: 'POST', headers: {'Content-Type': 'application/json'},
                                 body: JSON.stringify({seconds: 20})});
}
setInterval(async () => {
  try {
    const s = await (await fetch('/api/state')).json();
    const c = s.calibration;
    document.getElementById('calib').textContent = 'Calibration: ' + c.state + ' - ' + c.message +
      (c.state === 'running' ? ` (${Math.round((c.progress || 0) * 100)}%)` : '');
  } catch (e) {}
}, 1000);

async function setLabel() {
  const label = document.getElementById('label').value;
  await fetch('/api/label', {method: 'POST', headers: {'Content-Type': 'application/json'},
                             body: JSON.stringify({label})});
  refresh();
}

function drawAmplitudes(canvas, values) {
  const ctx = canvas.getContext('2d');
  const w = canvas.width = canvas.clientWidth * devicePixelRatio;
  const h = canvas.height = canvas.clientHeight * devicePixelRatio;
  ctx.clearRect(0, 0, w, h);
  if (!values.length) return;
  const max = Math.max(...values, 1);
  ctx.strokeStyle = '#2f6fd6'; ctx.lineWidth = 2 * devicePixelRatio;
  ctx.beginPath();
  values.forEach((v, i) => {
    const x = i / (values.length - 1) * w;
    const y = h - (v / max) * (h - 6) - 3;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
}

function nodeCard(n) {
  const cardId = 'link-' + n.receiver_id + '-' + n.tx_node_id + '-' + (n.band === '5 GHz' ? '5' : '24');
  let card = document.getElementById(cardId);
  if (!card) {
    card = document.createElement('div');
    card.className = 'card'; card.id = cardId;
    const who = n.tx_node_id === 0 ? 'Access point' : 'Node ' + n.tx_node_id;
    card.innerHTML = `<b>${who} &rarr; receiver ${n.receiver_id}</b>
      <span class="pill band">${n.band}</span> <span class="muted ip"></span>
      <div class="row"><span>Samples/s</span><b class="rate"></b></div>
      <div class="row"><span>RSSI</span><b class="rssi"></b></div>
      <div class="row"><span>Lost samples</span><b class="lost"></b></div>
      <div class="row"><span>Motion score</span><b class="motion"></b></div>
      <div class="meter"><div></div></div>
      <div class="muted">CSI amplitude per subcarrier</div>
      <canvas></canvas>`;
    document.getElementById('links').appendChild(card);
  }
  card.classList.toggle('offline', !n.online);
  card.querySelector('.ip').textContent = n.online ? '' : `(offline ${n.seconds_since_seen}s)`;
  card.querySelector('.rate').textContent = n.samples_per_sec;
  card.querySelector('.rssi').textContent = n.rssi === null ? '-' : n.rssi + ' dBm';
  card.querySelector('.lost').textContent = n.lost_samples;
  card.querySelector('.motion').textContent = n.motion === null ? '-' : n.motion;
  card.querySelector('.meter div').style.width = Math.min(100, (n.motion || 0) * 5) + '%';
  drawAmplitudes(card.querySelector('canvas'), n.latest_amplitudes);
}

async function refresh() {
  try {
    const d = await (await fetch('/api/links')).json();
    document.getElementById('csv').textContent = d.csv_file
      ? `Saving to ${d.csv_file} (${d.csv_rows} rows - only while a label is set or a guided session runs)` : 'CSV logging is off';
    const room = document.getElementById('room');
    room.textContent = d.room_state; room.className = 'pill ' + d.room_state;
    document.getElementById('current-label').textContent = d.label;
    const bandNote = d.bands.note ? `<span class="pill">${d.bands.note}</span>` : '';
    document.getElementById('bands').innerHTML = bandNote + ['2.4 GHz', '5 GHz'].map(band => [band, d.bands[band]]).map(([band, b]) =>
      `<span class="pill">${band}: ${b.links} link(s) &middot; ${b.samples_per_sec} samples/s` +
      ` &middot; avg RSSI ${b.avg_rssi ?? '-'} &middot; avg motion ${b.avg_motion ?? '-'}</span>`).join('');
    if (!d.links.length) {
      document.getElementById('links').innerHTML =
        '<div class="muted">No data yet. Are the receivers running and pointed at this Pi\'s IP?</div>';
    } else {
      const placeholder = document.querySelector('#links > .muted');
      if (placeholder) placeholder.remove();
      d.links.forEach(nodeCard);
    }
  } catch (e) { /* server restarting - try again next tick */ }
}
setInterval(refresh, 500);
refresh();
</script>
</body>
</html>
"""


@app.route("/")
def dashboard():
    return DASHBOARD_HTML


# ============================================================================
#  SECTION 8: START EVERYTHING
# ============================================================================

if __name__ == "__main__":
    # Hide Flask's per-request log lines (the dashboard polls twice a second)
    logging.getLogger("werkzeug").setLevel(logging.WARNING)

    if SAVE_TO_CSV:
        csv_logger = CsvLogger(DATA_DIR)

    threading.Thread(target=udp_listener, daemon=True).start()

    analyzer = Analyzer(ROOM_FILE, MODELS_DIR, DATA_DIR, links, links_lock, guided)
    print(f"[MODEL] {analyzer.model.status}")
    print(f"[CAL] Calibration: {analyzer.calib_status['message']}")
    threading.Thread(target=analyzer.run, daemon=True).start()

    print(f"[WEB] Dashboard at http://localhost:{WEB_PORT}/   3D view: /room   volunteers: /collect")

    # use_reloader=False: the reloader would start two copies that fight
    # over the UDP port.
    app.run(host=WEB_HOST, port=WEB_PORT, debug=False, use_reloader=False)

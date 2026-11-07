"""
============================================================================
 benchmark.py  -  how accurate is the people-finding? (on the simulator)
============================================================================
 Runs a fixed, repeatable scenario through the simulator (sim_room.py) and
 the REAL analysis code (analysis.py / localizer.py / csi_features.py),
 much faster than real time, and scores the result against the true
 positions. No server, no network, no hardware.

 Scenario (about 8 simulated minutes):
   empty room 22 s (calibration) -> empty 30 s -> 1 person walking 120 s
   -> 1 person standing still at 10 spots 120 s -> 2 people walking 90 s
   -> empty 30 s -> 1 person standing at 5 more spots 60 s

 Scores (geometry mode = no AI model, which is what runs before training):
   1 person: median / mean position error (m), zone right (3x3 grid),
             shown as exactly 1 person
   2 people: shown as exactly 2
   empty:    false alarms (anyone shown when the room is empty)

 USAGE
   python benchmark.py                               # room.json, interference on
   python benchmark.py --room bench/room_5tx_1rx.json --no-interference
   python benchmark.py --code ../some/other/server   # benchmark another code version
   python benchmark.py --all                         # the comparison table in the README

 The simulator is a simplified room: these numbers compare versions and
 layouts fairly with each other. They do NOT predict real-room accuracy.
============================================================================
"""

import argparse
import importlib.util
import json
import math
import os
import random
import subprocess
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

UPDATES_PER_SECOND = 4
RATE_HZ = 50
T0 = 1_000_000.0                 # simulated clock start (any value works)
SETTLE_S = 3.0                   # not scored right after the scenario changes


def load_sim():
    """The simulator always comes from THIS folder, so every version sees the same room."""
    spec = importlib.util.spec_from_file_location("bench_sim", os.path.join(HERE, "sim_room.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_scenario():
    """[(kind, people, seconds)]. kind: empty | walk | stand"""
    return [("calib", 0, 22), ("empty", 0, 30), ("walk", 1, 120), ("stand", 1, 120),
            ("walk", 2, 90), ("empty", 0, 30), ("stand", 1, 60)]


def make_sessions(out_dir, room_path, n_sessions, interference, seed=1, hold_s=15, move_s=5):
    """
    Write simulated GUIDED sessions (like /collect produces) as CSV files in
    out_dir, for train_model.py. Each session: empty room 30 s, 2 rounds of
    1 volunteer standing + walking in every zone, 1 round of 2 volunteers
    standing, empty room 15 s.
    """
    sys.path.insert(0, HERE)
    import server as srv                                # noqa: E402
    sim = load_sim()
    os.makedirs(out_dir, exist_ok=True)
    with open(room_path) as f:
        room = json.load(f)
    cols, rows = sim.room_zones(room)
    zones = [f"{chr(65 + c)}{r + 1}" for r in range(rows) for c in range(cols)]
    paths = []
    for k in range(n_sessions):
        random.seed(seed * 100 + k)
        np.random.seed(seed * 100 + k)
        rng = random.Random(seed * 7 + k)
        sid = f"sim{seed}{k:02d}"
        receivers = sim.SimReceivers(room, channel_24=1, channel_5=36, seed=7, loss=0.02,
                                     interference=interference)
        crowd = sim.Crowd(room)
        steps = [("empty", [], 30)]
        for _ in range(2):
            order = zones[:]
            rng.shuffle(order)
            for z in order:
                steps += [("stand", [z], hold_s), ("walk", [z], hold_s)]
        order = zones[:]
        rng.shuffle(order)
        for i, z in enumerate(order):
            other = order[(i + 4) % len(order)]
            steps.append(("stand", [z, other], hold_s))
        steps.append(("empty", [], 15))
        path = os.path.join(out_dir, f"csi_{sid}.csv")
        with open(path, "w", newline="") as f:
            import csv
            w = csv.writer(f)
            w.writerow(srv.CSV_COLUMNS)
            t = T0 + k * 10000
            for index, (act, occ, hold) in enumerate(steps):
                for phase, secs in (("move", 0 if act == "empty" else move_s), ("hold", hold)):
                    step = {"index": index, "act": act, "zones": occ, "phase": phase}
                    label = (f"guided;sess={sid};step={index};phase={phase};act={act};"
                             f"occ={'+'.join(occ) or 'none'};n={len(occ)}")
                    end = t + secs
                    while t < end:
                        crowd.follow(step, t) if occ else crowd.set_count(0)
                        crowd.move(1.0 / RATE_HZ, t)
                        bodies = [p.torso(t) for p in crowd.people]
                        for rid, smp in receivers.tick(bodies, t):
                            w.writerow([f"{t:.3f}", label, rid, smp["tx_node_id"], srv.band_of(smp["channel"]),
                                        "sim", 0, smp["tx_seq"], smp["rx_timestamp_us"], smp["rssi"],
                                        smp["noise_floor"], smp["channel"], smp["rx_format"],
                                        smp["first_word_invalid"], len(smp["csi"]),
                                        " ".join(map(str, smp["csi"]))])
                        t += 1.0 / RATE_HZ
        paths.append(path)
        print(f"[SESSIONS] wrote {path}", file=sys.stderr)
    return paths


def run(code_dir, room_path, interference, seed=1, verbose=False, models_dir=None):
    sys.path.insert(0, code_dir)
    import analysis                                     # noqa: E402  (the version under test)
    import server as srv                                # noqa: E402  (its LinkState)
    sim = load_sim()

    random.seed(seed)
    np.random.seed(seed)
    with open(room_path) as f:
        room = json.load(f)
    tmp = tempfile.mkdtemp(prefix="csi-bench-")
    links, links_lock = {}, threading.Lock()
    an = analysis.Analyzer(room_path, models_dir or tmp, tmp, links, links_lock, None)
    if verbose:
        print(f"  model: {an.model.status}", file=sys.stderr)
    an.calibration = None
    receivers = sim.SimReceivers(room, channel_24=1, channel_5=36, seed=7, loss=0.02,
                                 interference=interference)
    crowd = sim.Crowd(room)
    rng = random.Random(seed + 5)
    w, d = room["width"], room["depth"]

    def zone_of(x, y):
        return an.room.zone_of(x, y)

    scores = {"err1": [], "zone1": [], "count1": [], "stand_err": [], "count2": [],
              "false_empty": [], "detected1": []}
    t = T0
    dt = 1.0 / RATE_HZ
    next_analysis = t
    for kind, n, secs in build_scenario():
        phase_start = t
        crowd.set_count(n)
        stand_targets = []
        if kind == "stand":
            stand_targets = [(rng.uniform(0.7, w - 0.7), rng.uniform(0.7, d - 0.7)) for _ in range(int(secs // 12))]
        if kind == "calib":
            an.calib_job = {"start": t + 2.0, "end": t + 20.0, "source": "benchmark"}
        while t < phase_start + secs:
            # --- people ---
            if kind == "walk":
                crowd.wander_all()
            elif kind == "stand" and crowd.people:
                p = crowd.people[0]
                idx = min(int((t - phase_start) // 12), len(stand_targets) - 1)
                p.mode = "stand"
                p.speed = 0.8
                p.target = stand_targets[idx]
            crowd.move(dt, t)
            bodies = [p.torso(t) for p in crowd.people]
            # --- radio -> links ---
            for rid, s in receivers.tick(bodies, t):
                band = srv.band_of(s["channel"])
                key = (rid, s["tx_node_id"], band)
                link = links.get(key)
                if link is None:
                    link = links[key] = srv.LinkState(rid, s["tx_node_id"], band, "sim")
                link.add_sample(s, t)
            # --- analysis, 4x per second ---
            if t >= next_analysis:
                next_analysis += 1.0 / UPDATES_PER_SECOND
                an.step(t)
                if kind != "calib" and t - phase_start >= SETTLE_S:
                    shown = an.snapshot().get("people", [])
                    truth = [(p.x, p.y) for p in crowd.people]
                    if kind == "empty":
                        scores["false_empty"].append(len(shown) > 0)
                    elif n == 1:
                        tx, ty = truth[0]
                        scores["count1"].append(len(shown) == 1)
                        scores["detected1"].append(len(shown) >= 1)
                        if shown:
                            best = min(shown, key=lambda q: (q["x"] - tx) ** 2 + (q["y"] - ty) ** 2)
                            e = math.hypot(best["x"] - tx, best["y"] - ty)
                            scores["err1"].append(e)
                            scores["zone1"].append(zone_of(best["x"], best["y"]) == zone_of(tx, ty))
                            p = crowd.people[0]
                            if kind == "stand" and math.hypot(p.target[0] - p.x, p.target[1] - p.y) < 0.05:
                                scores["stand_err"].append(e)
                    elif n == 2:
                        scores["count2"].append(len(shown) == 2)
            t += dt
        if verbose:
            print(f"  ... {kind} x{n} done (calibrated: {bool(an.calibration)})", file=sys.stderr)

    def pct(v):
        return round(100.0 * float(np.mean(v)), 1) if v else None

    def med(v):
        return round(float(np.median(v)), 2) if v else None

    return {
        "mode": an.snapshot().get("mode"),
        "links": len(links),
        "err1_median_m": med(scores["err1"]),
        "err1_mean_m": round(float(np.mean(scores["err1"])), 2) if scores["err1"] else None,
        "stand_err_median_m": med(scores["stand_err"]),
        "zone1_pct": pct(scores["zone1"]),
        "detected1_pct": pct(scores["detected1"]),
        "count1_pct": pct(scores["count1"]),
        "count2_pct": pct(scores["count2"]),
        "false_alarm_empty_pct": pct(scores["false_empty"]),
    }


ALL_RUNS = [
    # (label, code, room, interference)
    ("flash-ready code, 5 TX + S3", "baseline", "bench/room_5tx_1rx.json", True),
    ("new code,         5 TX + S3", "new", "bench/room_5tx_1rx.json", True),
    ("new code,        10 TX + S3", "new", "bench/room_10tx_1rx.json", True),
    ("new code,        10 TX + S3 + C5 (5 GHz)", "new", "bench/room_10tx_s3_c5.json", True),
    ("new code,        10 TX + S3 + 2nd 2.4 GHz rx + C5", "new", "bench/room_10tx_2rx_c5.json", True),
    ("flash-ready code, 5 TX + S3 (quiet air)", "baseline", "bench/room_5tx_1rx.json", False),
    ("new code,        10 TX + S3 + C5 (quiet air)", "new", "bench/room_10tx_s3_c5.json", False),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--code", default=HERE, help="folder with the analysis code to test (default: this one)")
    ap.add_argument("--room", default=os.path.join(HERE, "room.json"))
    ap.add_argument("--no-interference", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--all", action="store_true", help="run the comparison table (needs --baseline)")
    ap.add_argument("--baseline", default=None, help="folder with the flash-ready server code, for --all")
    ap.add_argument("--seeds", type=int, default=2, help="--all: average over this many seeds")
    ap.add_argument("--json", action="store_true", help="print one JSON line (used by --all)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--models", default=None, help="use the AI model in this folder (from train_model.py)")
    ap.add_argument("--make-sessions", type=int, default=0, metavar="N",
                    help="write N simulated guided sessions (CSV) to --out, for train_model.py")
    ap.add_argument("--out", default=None, help="folder for --make-sessions")
    args = ap.parse_args()

    if args.make_sessions:
        if not args.out:
            sys.exit("--make-sessions needs --out <folder>")
        make_sessions(args.out, os.path.abspath(args.room), args.make_sessions,
                      not args.no_interference, args.seed)
        return

    if not args.all:
        res = run(os.path.abspath(args.code), os.path.abspath(args.room),
                  not args.no_interference, args.seed, args.verbose,
                  os.path.abspath(args.models) if args.models else None)
        if args.json:
            print(json.dumps(res))
        else:
            for k, v in res.items():
                print(f"{k:24s} {v}")
        return

    if not args.baseline:
        sys.exit("--all needs --baseline <folder with the flash-ready server code>")
    jobs = []
    for label, code, room, inter in ALL_RUNS:
        code_dir = os.path.abspath(args.baseline if code == "baseline" else HERE)
        for seed in range(1, args.seeds + 1):
            cmd = [sys.executable, os.path.abspath(__file__), "--json", "--code", code_dir,
                   "--room", os.path.join(HERE, room), "--seed", str(seed)]
            if not inter:
                cmd.append("--no-interference")
            jobs.append((label, subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)))
    results = {}
    for label, proc in jobs:
        out, _ = proc.communicate()
        results.setdefault(label, []).append(json.loads(out.strip().splitlines()[-1]))
    keys = ["err1_median_m", "stand_err_median_m", "zone1_pct", "count1_pct", "count2_pct",
            "false_alarm_empty_pct", "links"]
    print("| Setup | " + " | ".join(keys) + " |")
    print("|---|" + "---|" * len(keys))
    for label, *_ in ALL_RUNS:
        rows = results[label]
        vals = []
        for k in keys:
            v = [r[k] for r in rows if r[k] is not None]
            vals.append(f"{np.mean(v):.2f}" if v and k.endswith("_m") else (f"{np.mean(v):.0f}" if v else "-"))
        print(f"| {label} | " + " | ".join(vals) + " |")


if __name__ == "__main__":
    main()

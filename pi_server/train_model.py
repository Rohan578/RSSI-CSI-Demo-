"""
============================================================================
 train_model.py  -  teach the Pi which floor zones are occupied
============================================================================
 Reads the CSV files recorded during GUIDED sessions (/collect page), cuts
 them into 1-second windows, and trains a random forest that answers, for
 every zone A1..C3: "is someone standing here?". Several people at once are
 fine (one yes/no answer per zone).

 RUN IT ON THE PI (not on a laptop): the saved model only loads with the
 same scikit-learn version that trained it.

   cd ~/csi/pi_server
   python3 train_model.py                  # all guided sessions in data/
   python3 train_model.py --list           # show sessions found
   python3 train_model.py --sessions s20261105-1400 s20261105-1500
   then: dashboard -> 3D view, or  curl -X POST localhost:5000/api/model/reload

 HONEST ACCURACY
   It tests itself by hiding whole steps (one volunteer visit to one zone)
   from training and predicting them. Windows from the same visit are never
   split between training and testing, because neighbouring seconds look
   almost identical and would make the score meaninglessly high.
   For a real test, record two sessions and pass --test-session <id>: it
   then trains on the others and tests on that one only.

 WHAT MAKES IT WORK BETTER
   - 2+ rounds per session (every zone visited at least twice)
   - sessions on different days / with different volunteers
   - an empty-room step at the start (guided sessions always have one)
   - boards NEVER moved after recording
============================================================================
"""

import argparse
import csv
import glob
import json
import os
import sys
import time
from collections import defaultdict

import numpy as np

import csi_features as cf
from guided import parse_label
from localizer import Room

WINDOW_S = 1.0         # Must match analysis.WINDOW_S
HOP_S = 0.5            # New window every half second
SETTLE_S = 1.5         # Skip the first seconds of every hold (people still settling)
MIN_LINK_FRACTION = 0.6

csv.field_size_limit(10_000_000)
HERE = os.path.dirname(os.path.abspath(__file__))


def link_key(receiver_id, tx_node_id, band):
    return f"r{receiver_id}-t{tx_node_id}-{'5' if band == '5 GHz' else '24'}"


# ============================================================================
#  1. Read the guided rows
# ============================================================================

def load_sessions(paths, wanted=None):
    """
    -> {session: {"links": {key: [t, amp, rssi lists]}, "steps": {(step, phase): info}}}
    Only rows written while a guided session ran are used.
    """
    sessions = {}
    for path in paths:
        print(f"[READ] {path}")
        n = 0
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                info = parse_label(row["label"])
                if info is None or info.get("step") == "-1":
                    continue
                sess = info.get("sess")
                if wanted and sess not in wanted:
                    continue
                values = row["csi"].split(" ", 128)[:128]
                if len(values) < 128:
                    continue
                amp = cf.normalize(cf.lltf_amplitudes(np.array(values, dtype=np.int8),
                                                      row["first_word_invalid"] == "1"))
                if amp is None:
                    continue
                t = float(row["pi_time"])
                S = sessions.setdefault(sess, {"links": defaultdict(lambda: ([], [], [])), "steps": {}})
                key = link_key(row["receiver_id"], row["tx_node_id"], row["band"])
                L = S["links"][key]
                L[0].append(t)
                L[1].append(amp)
                L[2].append(float(row["rssi"]))
                sk = (int(info["step"]), info["phase"])
                st = S["steps"].setdefault(sk, {"act": info["act"], "zones": info["zones"],
                                                "start": t, "end": t})
                st["start"] = min(st["start"], t)
                st["end"] = max(st["end"], t)
                n += 1
        print(f"       {n} guided rows")
    for S in sessions.values():
        S["links"] = {k: (np.array(v[0]), np.array(v[1], dtype=np.float32), np.array(v[2], dtype=np.float32))
                      for k, v in S["links"].items()}
    return sessions


# ============================================================================
#  2. Windows -> features + labels
# ============================================================================

def session_baseline(S, fallback):
    """Empty-room reference from the session's FIRST empty step (as live does)."""
    empties = sorted(k for k, st in S["steps"].items() if k[1] == "hold" and st["act"] == "empty")
    if empties:
        st = S["steps"][empties[0]]
        base = {}
        for key, (t, a, r) in S["links"].items():
            m = (t >= st["start"] + 1.0) & (t <= st["end"] - 0.5)
            b = cf.build_baseline(t[m], a[m], r[m])
            if b is not None:
                base[key] = b
        if base:
            return base, "session empty step"
    return fallback, "data/calibration.json"


def build_dataset(sessions, zones, link_order, fallback_baseline):
    X, Y, groups, meta = [], [], [], []
    for sess, S in sorted(sessions.items()):
        base, source = session_baseline(S, fallback_baseline)
        if not base:
            print(f"[SKIP] {sess}: no empty-room reference")
            continue
        print(f"[DATA] {sess}: baseline from {source}, {len(base)} links")
        for (step, phase), st in sorted(S["steps"].items()):
            if phase != "hold":
                continue
            y = np.array([1 if z in st["zones"] else 0 for z in zones], dtype=np.int8)
            w0 = st["start"] + SETTLE_S
            while w0 + WINDOW_S <= st["end"]:
                per_link = {}
                for key in link_order:
                    if key not in S["links"] or key not in base:
                        continue
                    t, a, r = S["links"][key]
                    m = (t >= w0) & (t < w0 + WINDOW_S)
                    if m.sum() >= cf.MIN_WINDOW_SAMPLES:
                        per_link[key], _ = cf.link_features(a[m], r[m], base[key])
                if len(per_link) >= MIN_LINK_FRACTION * len(link_order):
                    X.append(cf.combined_features(per_link, link_order))
                    Y.append(y)
                    groups.append(f"{sess}/{step}")
                    meta.append({"sess": sess, "act": st["act"], "zones": st["zones"]})
                w0 += HOP_S
    if not X:
        return None, None, None, None
    return np.array(X), np.array(Y), np.array(groups), meta


# ============================================================================
#  3. Train + honest test
# ============================================================================

def make_model():
    """Per zone: is someone here? (one yes/no output per zone)"""
    from sklearn.ensemble import RandomForestClassifier
    return RandomForestClassifier(n_estimators=300, min_samples_leaf=2, max_features="sqrt",
                                  class_weight="balanced", n_jobs=-1, random_state=0)


def make_count_model():
    """How many people? (0, 1, 2, 3). Then the top-N zones are the answer:
    far more reliable for 2-3 people than thresholding each zone alone."""
    from sklearn.ensemble import RandomForestClassifier
    return RandomForestClassifier(n_estimators=300, min_samples_leaf=2, max_features="sqrt",
                                  class_weight="balanced", n_jobs=-1, random_state=0)


def count_probs(model, X, max_n=3):
    out = np.zeros((X.shape[0], max_n + 1))
    proba = model.predict_proba(X)
    for j, c in enumerate(model.classes_):
        if int(c) <= max_n:
            out[:, int(c)] = proba[:, j]
    return out


def pick_zones(P, C):
    """Top-N zones, N = most likely head count."""
    n = np.argmax(C, axis=1)
    pred = np.zeros(P.shape, dtype=bool)
    for i, k in enumerate(n):
        if k:
            pred[i, np.argsort(-P[i])[:k]] = True
    return pred


def occupied_probs(model, X):
    probs = model.predict_proba(X)
    out = np.zeros((X.shape[0], len(probs)))
    for j, (p, classes) in enumerate(zip(probs, model.classes_)):
        classes = list(classes)
        if 1 in classes:
            out[:, j] = p[:, classes.index(1)]
    return out


def score(P, C, Y, meta, zones):
    pred = pick_zones(P, C)
    truth = Y.astype(bool)
    n_true = truth.sum(axis=1)
    single = n_true == 1
    empty = n_true == 0
    res = {
        "windows": int(len(Y)),
        "exact_match": float(np.mean(np.all(pred == truth, axis=1))),
        "count_accuracy": float(np.mean(pred.sum(axis=1) == n_true)),
        "count_accuracy_by_n": {int(k): float(np.mean(pred[n_true == k].sum(axis=1) == k))
                                for k in sorted(set(n_true.tolist()))},
        "empty_accuracy": float(np.mean(pred[empty].sum(axis=1) == 0)) if empty.any() else None,
        "zone_accuracy": float(np.mean(np.argmax(P[single], axis=1) == np.argmax(Y[single], axis=1)))
        if single.any() else None,
    }
    # Neighbouring zone counts as "close"
    if single.any():
        names = np.array(zones)
        t_idx = np.argmax(Y[single], axis=1)
        p_idx = np.argmax(P[single], axis=1)
        def rc(i):
            z = names[i]
            return ord(z[0]) - 65, int(z[1:])
        near = [max(abs(rc(a)[0] - rc(b)[0]), abs(rc(a)[1] - rc(b)[1])) <= 1 for a, b in zip(t_idx, p_idx)]
        res["zone_or_neighbour_accuracy"] = float(np.mean(near))
        confusion = defaultdict(lambda: defaultdict(int))
        for a, b in zip(t_idx, p_idx):
            confusion[zones[a]][zones[b]] += 1
        res["confusion"] = {k: dict(v) for k, v in confusion.items()}
    return res


def print_score(title, res, zones):
    def pct(v):
        return "  -  " if v is None else f"{v:5.0%}"
    print(f"\n=== {title}  ({res['windows']} windows) ===")
    print(f"  single person, exact zone ......... {pct(res.get('zone_accuracy'))}")
    print(f"  single person, zone or neighbour .. {pct(res.get('zone_or_neighbour_accuracy'))}")
    print(f"  empty room recognised ............. {pct(res.get('empty_accuracy'))}")
    print(f"  number of people right ............ {pct(res.get('count_accuracy'))}"
          + "   (" + ", ".join(f"{k} in room: {v:.0%}" for k, v in res.get("count_accuracy_by_n", {}).items()) + ")")
    print(f"  every zone right (all windows) .... {pct(res.get('exact_match'))}")
    conf = res.get("confusion")
    if conf:
        print("\n  single-person confusion (rows = true zone, columns = predicted):")
        print("        " + " ".join(f"{z:>4}" for z in zones))
        for z in zones:
            row = conf.get(z, {})
            print(f"   {z:>4} " + " ".join(f"{row.get(c, 0):>4}" for c in zones))


def main():
    ap = argparse.ArgumentParser(description="Train the zone model from guided sessions")
    ap.add_argument("--data-dir", default=os.path.join(HERE, "data"))
    ap.add_argument("--room", default=os.path.join(HERE, "room.json"))
    ap.add_argument("--out", default=os.path.join(HERE, "models"))
    ap.add_argument("--sessions", nargs="*", help="only these session ids")
    ap.add_argument("--test-session", help="train on the others, test on this one")
    ap.add_argument("--list", action="store_true", help="list sessions and exit")
    args = ap.parse_args()

    try:
        import joblib
        import sklearn
        from sklearn.model_selection import GroupKFold
    except ImportError:
        sys.exit("scikit-learn is missing:  sudo apt install python3-sklearn")

    room = Room.load(args.room)
    zones = room.zone_names()
    paths = sorted(glob.glob(os.path.join(args.data_dir, "csi_*.csv")))
    if not paths:
        sys.exit(f"No CSV files in {args.data_dir}")
    wanted = set(args.sessions) if args.sessions else None
    if wanted and args.test_session:
        wanted.add(args.test_session)
    sessions = load_sessions(paths, wanted)
    if args.list or not sessions:
        for sess, S in sorted(sessions.items()):
            holds = [k for k in S["steps"] if k[1] == "hold"]
            print(f"  {sess}: {len(holds)} steps, links {sorted(S['links'])}")
        if not sessions:
            sys.exit("No guided-session rows found. Run a session on the /collect page first.")
        return

    fallback = {}
    cal_path = os.path.join(args.data_dir, "calibration.json")
    if os.path.exists(cal_path):
        with open(cal_path) as f:
            cal = json.load(f)
        if cal.get("feature_version") == cf.FEATURE_VERSION:
            fallback = cal.get("links", {})

    # Links that exist in the data AND have a position in room.json
    all_links = sorted({k for S in sessions.values() for k in S["links"]})
    link_order = [k for k in all_links
                  if room.link_segment(int(k.split("-")[0][1:]), int(k.split("-")[1][1:])) is not None]
    if not link_order:
        link_order = all_links
    print(f"[DATA] links used: {link_order}")

    X, Y, groups, meta = build_dataset(sessions, zones, link_order, fallback)
    if X is None:
        sys.exit("No usable windows (were the transmitters running during the session?)")
    print(f"[DATA] {len(X)} windows, {len(set(groups))} steps, {X.shape[1]} features")

    visits = defaultdict(int)
    step_zones = {g: m["zones"] for g, m in zip(groups, meta)}
    for zs in step_zones.values():
        for z in zs:
            visits[z] += 1
    thin = [z for z in zones if visits[z] < 2]
    if thin:
        print(f"[WARN] zones visited fewer than 2 times: {thin}. "
              "Record more rounds - the self-test can't judge these.")

    metrics = {}
    if args.test_session:
        tr = np.array([m["sess"] != args.test_session for m in meta])
        if tr.all() or not tr.any():
            sys.exit(f"--test-session {args.test_session}: need it AND at least one other session")
        model = make_model().fit(X[tr], Y[tr])
        counter = make_count_model().fit(X[tr], Y[tr].sum(axis=1))
        metrics = score(occupied_probs(model, X[~tr]), count_probs(counter, X[~tr]), Y[~tr],
                        [m for m, k in zip(meta, ~tr) if k], zones)
        print_score(f"Trained on other sessions, tested on {args.test_session}", metrics, zones)
    else:
        n_groups = len(set(groups))
        k = min(5, n_groups)
        if k >= 2:
            P = np.zeros(Y.shape)
            C = np.zeros((len(Y), 4))
            for tr, te in GroupKFold(n_splits=k).split(X, Y, groups):
                model = make_model().fit(X[tr], Y[tr])
                counter = make_count_model().fit(X[tr], Y[tr].sum(axis=1))
                P[te] = occupied_probs(model, X[te])
                C[te] = count_probs(counter, X[te])
            metrics = score(P, C, Y, meta, zones)
            print_score(f"Self-test: {k}-fold, whole steps held out", metrics, zones)
        else:
            print("[WARN] Too few steps for a self-test.")

    print("\n[TRAIN] Fitting the final model on all windows...")
    model = make_model().fit(X, Y)
    counter = make_count_model().fit(X, Y.sum(axis=1))
    os.makedirs(args.out, exist_ok=True)
    joblib.dump({"zones": model, "count": counter}, os.path.join(args.out, "zone_model.joblib"))
    info = {
        "feature_version": cf.FEATURE_VERSION,
        "sklearn_version": sklearn.__version__,
        "trained_at": time.strftime("%Y-%m-%d %H:%M"),
        "zones": zones,
        "links": link_order,
        "windows": int(len(X)),
        "sessions": sorted(sessions),
        "window_s": WINDOW_S,
        "max_people": int(Y.sum(axis=1).max()),
        "metrics": {k: v for k, v in metrics.items() if k != "confusion"},
    }
    with open(os.path.join(args.out, "zone_model.json"), "w") as f:
        json.dump(info, f, indent=1)
    print(f"[TRAIN] Saved {args.out}/zone_model.joblib")
    print("        Load it: restart the server, or  curl -X POST localhost:5000/api/model/reload")


if __name__ == "__main__":
    main()

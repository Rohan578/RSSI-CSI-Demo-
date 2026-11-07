"""
============================================================================
 analysis.py  -  the live "what's happening in the room" engine
============================================================================
 Runs in a background thread, UPDATES_PER_SECOND times a second:

   1. Takes the last WINDOW_S seconds of CSI from every link.
   2. Scores each link 0..1 "how disturbed" against the empty-room
      calibration (or, before calibration, against its own quiet periods).
   3. Asks the localizer where people are (geometry, or the AI model once
      one has been trained), then the tracker smooths that into numbered
      people.
   4. Publishes one snapshot that /api/state hands to the 3D page.

 CALIBRATION = recording the EMPTY room for ~20 s. Needed for:
   - spotting people who stand still (the shape changes, nothing wobbles)
   - the AI model (its features are measured relative to the empty room)
 It's saved to data/calibration.json and reused after restarts. Re-do it
 whenever furniture moves or the readings look "busy" with nobody there.
 Guided sessions (/collect) re-calibrate automatically at their first step.
============================================================================
"""

import base64
import json
import os
import threading
import time
from collections import deque

import numpy as np

import csi_features as cf
from localizer import GeometricLocalizer, Room, Tracker, ZoneModel, fade_weights

# ============================================================================
#  SETTINGS
# ============================================================================

UPDATES_PER_SECOND = 4
WINDOW_S = 1.0               # Seconds of CSI per decision (must match train_model.py)
LINK_STALE_S = 3.0           # Link counts as offline after this long silent
CALIBRATION_S = 20           # Default empty-room recording length
COMMON_MODE_MIN_LINKS = 4    # Links a receiver needs before interference is subtracted
INTERFERENCE_LEVEL = 0.25    # Common-mode disturbance counted as "interference"
FIVE_GHZ_CONFIRM = 0.30      # 5 GHz presence that overrides the interference gate
SIM_TRUTH_MAX_AGE_S = 2.0


def link_key(receiver_id, tx_node_id, band):
    return f"r{receiver_id}-t{tx_node_id}-{'5' if band == '5 GHz' else '24'}"


class Analyzer:
    def __init__(self, room_path, models_dir, data_dir, links, links_lock, guided):
        self.room_path = room_path
        self.room = Room.load(room_path)
        self.geo = GeometricLocalizer(self.room)
        self.tracker = Tracker()
        self.model = ZoneModel(models_dir)
        self.model.load()
        self.links = links                  # server.py's dict of LinkState
        self.links_lock = links_lock
        self.guided = guided
        self.calib_path = os.path.join(data_dir, "calibration.json")
        self.calibration = self._load_calibration()
        self.calib_job = None
        self.calib_status = ({"state": "ok", "message": "loaded from disk"}
                             if self.calibration else
                             {"state": "none", "message": "not calibrated yet"})
        self.lock = threading.Lock()
        self.state = {"t": 0, "links": [], "people": []}
        self.motion_history = {}            # key -> deque, for the uncalibrated fallback
        self.sim_truth = None               # (time, [ {id,x,y} ])
        self.sim_stats = deque(maxlen=240)  # last ~60 s of (error_m, count_ok, zone_ok)
        self._done_empty_holds = set()

    # ------------------------------------------------------------------
    #  Calibration
    # ------------------------------------------------------------------
    def _load_calibration(self):
        try:
            with open(self.calib_path) as f:
                data = json.load(f)
            if data.get("feature_version") == cf.FEATURE_VERSION and data.get("links"):
                return data
        except (OSError, ValueError):
            pass
        return None

    def start_calibration(self, seconds=CALIBRATION_S, source="button"):
        now = time.time()
        seconds = max(8.0, min(55.0, float(seconds)))
        with self.lock:
            self.calib_job = {"start": now, "end": now + seconds, "source": source}
            self.calib_status = {"state": "running", "message": f"recording empty room ({source})",
                                 "progress": 0.0}

    def _finish_calibration(self, job, now):
        result = {}
        with self.links_lock:
            for (rx, tx, band), link in self.links.items():
                t, a, r = link.buf.since(job["start"])
                keep = t <= job["end"]
                base = cf.build_baseline(t[keep], a[keep], r[keep])
                if base is not None:
                    result[link_key(rx, tx, band)] = base
        if not result:
            self.calib_status = {"state": "failed",
                                 "message": "no CSI arrived while calibrating - is the S3 sending?"}
            return
        data = {"feature_version": cf.FEATURE_VERSION, "created": now,
                "source": job["source"], "links": result}
        try:
            tmp = self.calib_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(data, f)
            os.replace(tmp, self.calib_path)
        except OSError as error:
            print(f"[CAL] Could not save calibration: {error}")
        self.calibration = data
        self.tracker.clear()
        # An empty room is steady. Lots of wobble while "calibrating" usually
        # means someone was still in the room (or walked past the door).
        busy = [k for k, v in result.items() if v["motion_sigma"] > 0.5 * max(v["motion_mu"], 0.5)
                or v["dev_sigma"] > 2.5]
        msg = f"{len(result)} link(s) calibrated ({job['source']})"
        if busy:
            msg += (f" - WARNING: {', '.join(busy)} looked busy. Was the room really empty? "
                    "If not, calibrate again.")
        self.calib_status = {"state": "ok", "message": msg, "busy_links": busy}
        print(f"[CAL] Empty-room calibration done: {len(result)} links ({job['source']})")

    # ------------------------------------------------------------------
    #  Simulator ground truth (only sim_room.py posts this)
    # ------------------------------------------------------------------
    def set_sim_truth(self, people):
        with self.lock:
            self.sim_truth = (time.time(), people)

    def reload(self):
        """Re-read room.json and the model (after editing / training)."""
        room = Room.load(self.room_path)
        with self.lock:
            self.room = room
            self.geo = GeometricLocalizer(room)
            self.tracker.clear()
        ok = self.model.load()
        return ok

    # ------------------------------------------------------------------
    #  Main loop
    # ------------------------------------------------------------------
    def run(self):
        period = 1.0 / UPDATES_PER_SECOND
        next_t = time.time()
        while True:
            try:
                self.step(time.time())
            except Exception as error:               # never let the thread die
                print(f"[ANALYSIS] Error: {error!r}")
            next_t += period
            time.sleep(max(0.0, next_t - time.time()))
            if time.time() - next_t > 2:              # fell far behind: resync
                next_t = time.time()

    def step(self, now):
        # --- 0. Calibration bookkeeping -------------------------------------
        hold = self.guided.current_empty_hold(now) if self.guided else None
        if hold and hold[0] not in self._done_empty_holds and self.calib_job is None:
            start, end = hold
            if end - start >= 10:
                self._done_empty_holds.add(hold[0])
                with self.lock:
                    self.calib_job = {"start": start + 1.0, "end": end - 0.5, "source": "guided session"}
                    self.calib_status = {"state": "running", "progress": 0.0,
                                         "message": "recording empty room (guided session)"}
        job = self.calib_job
        if job is not None:
            if now >= job["end"]:
                self._finish_calibration(job, now)
                self.calib_job = None
            else:
                self.calib_status["progress"] = round((now - job["start"]) / (job["end"] - job["start"]), 2)

        # --- 1. Grab each link's last window ---------------------------------
        snap = []
        with self.links_lock:
            for (rx, tx, band), link in self.links.items():
                t, a, r = link.buf.since(now - WINDOW_S)
                snap.append((rx, tx, band, link, t, a, r))
        calib = self.calibration["links"] if self.calibration else {}
        model_ready = self.model.ready and bool(calib)
        weights = fade_weights([(rx, tx, band, calib[link_key(rx, tx, band)]["rssi"])
                                for rx, tx, band, *_ in snap if link_key(rx, tx, band) in calib],
                               self.room)

        # --- 2. Score every link ----------------------------------------------
        link_rows, segments, strengths, motion_s, per_link_vec = [], [], [], [], {}
        for rx, tx, band, link, t, a, r in snap:
            key = link_key(rx, tx, band)
            online = (now - link.last_seen) < LINK_STALE_S and len(t) >= cf.MIN_WINDOW_SAMPLES
            row = {"key": key, "receiver_id": rx, "tx_node_id": tx, "band": band,
                   "online": online, "rate": round(len(t) / WINDOW_S, 1),
                   "rssi": link.last_rssi, "lost": link.lost_samples,
                   "calibrated": key in calib, "has_position": False,
                   "motion": None, "deviation": None, "drssi": None,
                   "disturbance": 0.0, "presence": 0.0, "moving": 0.0,
                   "layout_ok": link.layout_ok}
            if online:
                mean, _std, motion = cf.window_stats(a)
                rssi_std = float(r.std())
                row["motion"] = round(motion, 2)
                base = calib.get(key)
                if base is not None:
                    vec, summary = cf.link_features(a, r, base)
                    per_link_vec[key] = vec
                    row["deviation"] = round(summary["deviation"], 2)
                    row["drssi"] = round(summary["drssi"], 2)
                    # "Someone is in the room" (shape/wobble: not local)
                    s_dev = cf.disturbance(summary["deviation"], base["dev_mu"], base["dev_sigma"])
                    s_mot = cf.disturbance(motion, base["motion_mu"], base["motion_sigma"])
                    # "Someone is near THIS path" (power change/wobble: local)
                    s_att = cf.rssi_change(summary["drssi"], base.get("rssi_jitter", 0.3))
                    s_rvar = cf.disturbance(rssi_std, base.get("rssi_std_mu", 0.3),
                                            base.get("rssi_std_sigma", 0.2), z0=2.5, scale=5.0)
                    s_local = max(s_att, s_rvar) * weights.get((rx, tx, band), 1.0)
                    row["reliability"] = round(weights.get((rx, tx, band), 1.0), 2)
                else:
                    # Not calibrated: compare against this link's own quietest moments
                    hist = self.motion_history.setdefault(key, deque(maxlen=240))
                    hist.append((motion, rssi_std))
                    arr = np.array(hist)
                    m_floor = float(np.percentile(arr[:, 0], 10)) if len(hist) >= 20 else motion
                    r_floor = float(np.percentile(arr[:, 1], 10)) if len(hist) >= 20 else rssi_std
                    s_dev = 0.0
                    s_mot = cf.disturbance(motion, m_floor, max(0.3, 0.15 * m_floor))
                    s_local = cf.disturbance(rssi_std, r_floor, 0.25, z0=2.5, scale=5.0)
                row["presence"] = round(max(s_dev, s_mot), 3)
                row["disturbance"] = round(s_local, 3)
                row["moving"] = round(s_mot, 3)
                seg = self.room.link_segment(rx, tx)
                if seg is not None:
                    row["has_position"] = True
                    row["_seg"] = seg
            link_rows.append(row)
        link_rows.sort(key=lambda x: (x["receiver_id"], x["tx_node_id"], x["band"]))

        # --- 2b. Remove "common mode" disturbance (home-WiFi interference) ---
        # Other devices' 2.4 GHz traffic hits EVERY link of a receiver at the
        # same moment; a person only changes the few links whose paths pass
        # near them. So per receiver and band, the median link's disturbance
        # is interference, not people: subtract it. (Needs >= COMMON_MODE_MIN_LINKS
        # links - with fewer, a person's links could be the median.)
        common = {}
        for row in link_rows:
            if row.get("_seg") is not None and row["online"]:
                common.setdefault((row["receiver_id"], row["band"]), []).append(row["disturbance"])
        cm_level = {k: (float(np.median(v)) if len(v) >= COMMON_MODE_MIN_LINKS else 0.0)
                    for k, v in common.items()}
        for row in link_rows:
            seg = row.pop("_seg", None)
            if seg is None:
                continue
            cm = cm_level.get((row["receiver_id"], row["band"]), 0.0)
            row["raw_disturbance"] = row["disturbance"]
            row["disturbance"] = round(max(0.0, row["disturbance"] - cm), 3)
            segments.append(seg)
            strengths.append(row["disturbance"])
            motion_s.append(row["moving"])
        interference = {f"r{rx}-{'5' if band == '5 GHz' else '24'}": round(v, 3)
                        for (rx, band), v in cm_level.items() if v >= INTERFERENCE_LEVEL}

        # --- 3. Where are people? -----------------------------------------
        with self.lock:
            geo, room = self.geo, self.room
        detections, heat = geo.locate(segments, strengths)
        zone_probs, mode = None, "geometry"
        if model_ready:
            try:
                X = cf.combined_features(per_link_vec, self.model.link_order())
                zone_probs, count_probs = self.model.predict(X)
                mode = "model"
                zones = self.model.occupied(zone_probs, count_probs)
                detections = []
                for z in zones:
                    x, y = geo.best_in_zone(heat, z)
                    detections.append((x, y, zone_probs[z]))
                # Fade the zone glow by "is anyone here at all?" so an empty
                # room doesn't show faint blobs from low per-zone guesses
                anyone = 1.0
                if self.model.count_avg is not None:
                    anyone = 1.0 - float(self.model.count_avg[0])
                heat = 0.5 * heat + 0.5 * anyone * self._zone_heat(geo, room, zone_probs)
            except Exception as error:
                print(f"[MODEL] Prediction failed, using geometry: {error!r}")
                mode = "geometry"
        # Interference gate: while a 2.4 GHz receiver's links are all
        # disturbed together, don't let a NEW person appear - unless the
        # 5 GHz link (which that interference barely touches) also sees
        # something. People already shown keep being tracked.
        busy_24 = any(k.endswith("-24") for k in interference)
        five = [x["presence"] for x in link_rows if x["online"] and x["band"] == "5 GHz" and x["calibrated"]]
        five_sees = bool(five) and max(five) >= FIVE_GHZ_CONFIRM
        allow_new = (not busy_24) or five_sees
        tracks = self.tracker.update(detections, allow_new=allow_new)

        people = []
        for tr in tracks:
            moving = 0.0
            for (a_pt, b_pt), m in zip(segments, motion_s):
                d = _point_segment(tr["x"], tr["y"], a_pt, b_pt)
                if d < 0.8:
                    moving = max(moving, m)
            people.append({"id": tr["id"], "x": round(tr["x"], 2), "y": round(tr["y"], 2),
                           "zone": room.zone_of(tr["x"], tr["y"]),
                           "confidence": round(float(tr["strength"]), 2),
                           "moving": moving > 0.25})

        # --- 4. Simulator scoring --------------------------------------------
        with self.lock:
            truth = self.sim_truth
        sim = None
        if truth and now - truth[0] < SIM_TRUTH_MAX_AGE_S:
            sim = self._score_sim(truth[1], people, room)

        heat_u8 = (np.clip(heat, 0, 1) * 255).astype(np.uint8)
        state = {
            "t": now,
            "mode": mode,
            "model_status": self.model.status,
            "calibration": dict(self.calib_status,
                                created=self.calibration.get("created") if self.calibration else None),
            "links": link_rows,
            "links_online": sum(1 for x in link_rows if x["online"]),
            "layout_warning": [x["key"] for x in link_rows if x["layout_ok"] is False],
            "interference": interference,
            "new_people_paused": not allow_new,
            "activity": round(max([x.get("presence", 0.0) for x in link_rows if x["online"]] or [0.0]), 3),
            "people": people,
            "zone_probs": None if zone_probs is None else {k: round(v, 3) for k, v in zone_probs.items()},
            "heat": {"nx": geo.nx, "ny": geo.ny,
                     "data": base64.b64encode(heat_u8.tobytes()).decode("ascii")},
            "sim": sim,
        }
        with self.lock:
            self.state = state

    # ------------------------------------------------------------------
    def _zone_heat(self, geo, room, probs):
        out = np.zeros_like(geo.gx)
        zw = room.width / room.cols
        for z, p in probs.items():
            cx, cy = room.zone_center(z)
            out = np.maximum(out, p * np.exp(-((geo.gx - cx) ** 2 + (geo.gy - cy) ** 2) / (2 * (0.35 * zw) ** 2)))
        return out

    def _score_sim(self, truth, people, room):
        remaining = list(people)
        errors = []
        zone_hits = []
        for tp in truth:
            if remaining:
                best = min(remaining, key=lambda p: (p["x"] - tp["x"]) ** 2 + (p["y"] - tp["y"]) ** 2)
                remaining.remove(best)
                errors.append(((best["x"] - tp["x"]) ** 2 + (best["y"] - tp["y"]) ** 2) ** 0.5)
                zone_hits.append(best["zone"] == room.zone_of(tp["x"], tp["y"]))
        count_ok = len(truth) == len(people)
        self.sim_stats.append((errors, count_ok, zone_hits))
        all_err = [e for errs, _, _ in self.sim_stats for e in errs]
        all_zone = [z for _, _, zs in self.sim_stats for z in zs]
        return {
            "truth": truth,
            "error_m": round(float(np.mean(all_err)), 2) if all_err else None,
            "count_accuracy": round(float(np.mean([c for _, c, _ in self.sim_stats])), 2),
            "zone_accuracy": round(float(np.mean(all_zone)), 2) if all_zone else None,
        }

    def snapshot(self):
        with self.lock:
            return self.state


def _point_segment(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 < 1e-9 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    return ((px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2) ** 0.5

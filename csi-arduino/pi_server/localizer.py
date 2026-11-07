"""
============================================================================
 localizer.py  -  where are the people?  (room geometry + two estimators)
============================================================================

 1. GEOMETRY ESTIMATE (works right away, needs only room.json + a calibration)
    Every link is a straight radio path from a transmitter to the receiver.
    A person standing on or near that path disturbs it the most. For every
    10 cm floor cell we ask: "if one person stood HERE, which links would be
    disturbed?" and compare that pattern with what the links actually show.
    The best-matching cells form the heat map; the peak is the person.
    Then we explain that person's share away and look for another one.

    Honest limits: with ONE receiver all paths meet at the receiver, so
    position ALONG a path is the weakest part (side-to-side is better).
    People far from every path barely register.

 2. AI MODEL ESTIMATE (after train_model.py has learned from volunteer data)
    A random forest that learned what the CSI looks like with people in each
    floor zone (A1..C3). It uses the full subcarrier "fingerprint" of every
    link, including reflections, so it also sees people away from the paths.
    The geometry estimate is then used only to fine-tune the position
    inside each occupied zone.
============================================================================
"""

import json
import math
import os
import string

import numpy as np

import csi_features

# ============================================================================
#  SETTINGS
# ============================================================================

CELL_M = 0.10            # Heat-map cell size
PATH_WIDTH_M = 0.55      # How far from a path a person still disturbs it
SHARPNESS = 14.0         # Higher = tighter heat-map blobs
WALL_MARGIN_M = 0.30     # People can't stand inside the walls
MIN_STRENGTH = 0.22      # Weakest disturbance counted as a person (0..1)
EXTRA_PERSON_STRENGTH = 0.70   # ...and for a 2nd/3rd person (stricter: fewer ghosts)
EXTRA_PERSON_GAP_M = 2.0 # ...who must also be this far from the people already found
CENTROID_RADIUS_M = 2.5  # Area averaged around the best cell
MAX_PEOPLE = 3           # The model / tracker never shows more than this

TRACK_GATE_M = 1.6       # A detection this close continues an existing person
TRACK_SMOOTH = 0.30      # 0..1, how fast a person's marker follows (per update)
TRACK_CONFIRM = 2        # Updates in a row before the FIRST person is shown
TRACK_CONFIRM_EXTRA = 5  # ...and before a 2nd / 3rd person is shown
TRACK_DROP = 8           # Updates missing before a person is removed

ZONE_ON = 0.55           # AI model: zone counts as occupied above this...
ZONE_OFF = 0.40          # ...and stays occupied until it drops below this


# ============================================================================
#  ROOM
# ============================================================================

class Room:
    def __init__(self, cfg):
        self.cfg = cfg
        self.width = float(cfg["width"])
        self.depth = float(cfg["depth"])
        self.height = float(cfg.get("height", 2.4))
        z = cfg.get("zones", {})
        self.cols = int(z.get("cols", 3))
        self.rows = int(z.get("rows", 3))
        self.receivers = {int(k): v for k, v in cfg.get("receivers", {}).items()}
        self.transmitters = {int(k): v for k, v in cfg.get("transmitters", {}).items()}

    @classmethod
    def load(cls, path):
        with open(path) as f:
            return cls(json.load(f))

    # --- zones: columns A, B, C... from west; rows 1, 2, 3... from south ---
    def zone_names(self):
        return [f"{string.ascii_uppercase[c]}{r + 1}"
                for r in range(self.rows) for c in range(self.cols)]

    def zone_of(self, x, y):
        c = min(self.cols - 1, max(0, int(x / self.width * self.cols)))
        r = min(self.rows - 1, max(0, int(y / self.depth * self.rows)))
        return f"{string.ascii_uppercase[c]}{r + 1}"

    def zone_bounds(self, name):
        c = string.ascii_uppercase.index(name[0])
        r = int(name[1:]) - 1
        zw, zd = self.width / self.cols, self.depth / self.rows
        return c * zw, r * zd, (c + 1) * zw, (r + 1) * zd

    def zone_center(self, name):
        x0, y0, x1, y1 = self.zone_bounds(name)
        return (x0 + x1) / 2, (y0 + y1) / 2

    def link_segment(self, receiver_id, tx_node_id):
        """((tx_x, tx_y), (rx_x, rx_y)) or None if either end isn't in room.json."""
        rx = self.receivers.get(receiver_id)
        tx = self.transmitters.get(tx_node_id)
        if rx is None or tx is None:
            return None
        return (float(tx["x"]), float(tx["y"])), (float(rx["x"]), float(rx["y"]))


# ============================================================================
#  GEOMETRY ESTIMATE
# ============================================================================

def _dist_to_segment(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    length2 = dx * dx + dy * dy
    if length2 < 1e-9:
        return np.hypot(px - ax, py - ay)
    t = np.clip(((px - ax) * dx + (py - ay) * dy) / length2, 0.0, 1.0)
    return np.hypot(px - (ax + t * dx), py - (ay + t * dy))


def fade_weights(links, room, path_loss_exponent=2.0):
    """
    Links "in a deep fade" (much weaker than their length predicts, because
    reflections cancel the direct signal) jump around whenever anyone moves
    anywhere, so they're poor at saying WHERE. Weight them down.
    links: [(receiver_id, tx_node_id, empty_room_rssi)]. Returns {(rx, tx): 0.25..1}.
    """
    pts = []
    for rx, tx, rssi in links:
        seg = room.link_segment(rx, tx)
        if seg is None or rssi is None:
            continue
        d = max(0.5, math.hypot(seg[0][0] - seg[1][0], seg[0][1] - seg[1][1]))
        pts.append((rx, tx, rssi, 10 * path_loss_exponent * math.log10(d)))
    if len(pts) < 3:
        return {}
    p0 = float(np.median([rssi + loss for _, _, rssi, loss in pts]))
    out = {}
    for rx, tx, rssi, loss in pts:
        fade = rssi - (p0 - loss)                   # dB above (+) / below (-) the model
        out[(rx, tx)] = float(np.clip(1.0 + min(0.0, fade) / 12.0, 0.25, 1.0))
    return out


class GeometricLocalizer:
    def __init__(self, room):
        self.room = room
        self.nx = max(2, int(round(room.width / CELL_M)))
        self.ny = max(2, int(round(room.depth / CELL_M)))
        xs = (np.arange(self.nx) + 0.5) * room.width / self.nx
        ys = (np.arange(self.ny) + 0.5) * room.depth / self.ny
        self.gx, self.gy = np.meshgrid(xs, ys)          # shape (ny, nx)
        inside = ((self.gx > WALL_MARGIN_M) & (self.gx < room.width - WALL_MARGIN_M) &
                  (self.gy > WALL_MARGIN_M) & (self.gy < room.depth - WALL_MARGIN_M))
        self.prior = np.where(inside, 1.0, 0.05).ravel()
        self._cache_key = None
        self._E = None

    def _influence(self, segments):
        """(n_links, n_cells): expected disturbance of each link per person position."""
        key = tuple(segments)
        if key != self._cache_key:
            rows = []
            for a, b in segments:
                d = _dist_to_segment(self.gx, self.gy, a, b)
                rows.append(np.exp(-(d / PATH_WIDTH_M) ** 2).ravel())
            self._E = np.array(rows) if rows else np.zeros((0, self.nx * self.ny))
            self._cache_key = key
        return self._E

    def locate(self, segments, strengths, max_people=MAX_PEOPLE):
        """
        segments: list of link paths; strengths: matching 0..1 disturbances.
        Returns (detections [(x, y, strength)], heat (ny, nx) 0..1).
        """
        heat = np.zeros((self.ny, self.nx))
        if len(segments) < 2:
            return [], heat
        E = self._influence(segments)
        s = np.asarray(strengths, dtype=np.float64)
        enorm = np.linalg.norm(E, axis=0) + 1e-9
        detections = []
        for k in range(max_people):
            if s.max() < MIN_STRENGTH:
                break
            cos = (s @ E) / (enorm * (np.linalg.norm(s) + 1e-9))
            like = np.exp(SHARPNESS * (cos - 1.0)) * self.prior
            if k == 0:
                heat = (like / max(like.max(), 1e-9)).reshape(self.ny, self.nx)
                heat *= min(1.0, s.max() / 0.6)          # fade when signals are weak
            i = int(np.argmax(like))
            e = E[:, i]
            amount = float(s @ e) / float(e @ e + 1e-9)
            if amount < (MIN_STRENGTH if k == 0 else EXTRA_PERSON_STRENGTH):
                break
            # Centre of the best-matching area, not its single best cell: when
            # only one path is disturbed, every point along it fits equally
            # well, and the middle is the honest guess.
            gx, gy = self.gx.ravel(), self.gy.ravel()
            near = (np.hypot(gx - gx[i], gy - gy[i]) < CENTROID_RADIUS_M) & (like >= 0.6 * like[i])
            w = like[near]
            x = float((gx[near] * w).sum() / w.sum())
            y = float((gy[near] * w).sum() / w.sum())
            if any(math.hypot(x - dx, y - dy) < EXTRA_PERSON_GAP_M for dx, dy, _ in detections):
                break                          # same person's leftovers, not a new one
            detections.append((x, y, min(amount, 1.0)))
            s = np.clip(s - amount * e, 0.0, None)
        return detections, heat

    def best_in_zone(self, heat, zone):
        """Highest heat-map point inside a zone, or the zone centre if it's flat."""
        x0, y0, x1, y1 = self.room.zone_bounds(zone)
        m = (self.gx >= x0) & (self.gx < x1) & (self.gy >= y0) & (self.gy < y1)
        h = np.where(m, heat, -1.0)
        i = int(np.argmax(h))
        if h.ravel()[i] < 0.15:
            return self.room.zone_center(zone)
        return float(self.gx.ravel()[i]), float(self.gy.ravel()[i])


# ============================================================================
#  TRACKER  -  turns noisy detections into steady, numbered people
# ============================================================================

class Tracker:
    def __init__(self):
        self.tracks = []          # dicts: id x y strength hits misses confirmed

    def _free_id(self):
        # Numbers go only to people actually shown, so one person in the
        # room is "Person 1" even if a few brief false starts came before.
        used = {t["id"] for t in self.tracks if t["confirmed"]}
        i = 1
        while i in used:
            i += 1
        return i

    def update(self, detections):
        unmatched = list(range(len(detections)))
        for tr in sorted(self.tracks, key=lambda t: -t["hits"]):
            best, best_d = None, TRACK_GATE_M
            for j in unmatched:
                x, y, _ = detections[j]
                d = math.hypot(x - tr["x"], y - tr["y"])
                if d < best_d:
                    best, best_d = j, d
            if best is None:
                tr["misses"] += 1
                continue
            x, y, st = detections[best]
            unmatched.remove(best)
            a = TRACK_SMOOTH
            tr["x"] += a * (x - tr["x"])
            tr["y"] += a * (y - tr["y"])
            tr["strength"] += 0.4 * (st - tr["strength"])
            tr["hits"] += 1
            tr["misses"] = 0
            others = sum(1 for o in self.tracks if o is not tr and o["confirmed"])
            if not tr["confirmed"] and tr["hits"] >= (TRACK_CONFIRM if others == 0 else TRACK_CONFIRM_EXTRA):
                tr["id"] = self._free_id()
                tr["confirmed"] = True
        # One person shown, one detection left over (it jumped further than the
        # gate): it's almost certainly the same person - glide, don't renumber.
        stale = [t for t in self.tracks if t["misses"] > 0 and t["confirmed"]]
        if len(unmatched) == 1 and len(stale) == 1 and len(detections) == 1:
            tr = stale[0]
            x, y, st = detections[unmatched.pop()]
            tr["x"] += TRACK_SMOOTH * (x - tr["x"])
            tr["y"] += TRACK_SMOOTH * (y - tr["y"])
            tr["strength"] += 0.4 * (st - tr["strength"])
            tr["hits"] += 1
            tr["misses"] = 0
        for j in unmatched:
            if len(self.tracks) >= MAX_PEOPLE + 2:
                break
            x, y, st = detections[j]
            self.tracks.append({"id": self._free_id() if TRACK_CONFIRM <= 1 else 0,
                                "x": x, "y": y, "strength": st,
                                "hits": 1, "misses": 0, "confirmed": TRACK_CONFIRM <= 1})
        self.tracks = [t for t in self.tracks if t["misses"] <= TRACK_DROP]
        shown = [t for t in self.tracks if t["confirmed"]]
        shown.sort(key=lambda t: -t["strength"])
        return shown[:MAX_PEOPLE]

    def clear(self):
        self.tracks = []


# ============================================================================
#  AI MODEL  (trained by train_model.py)
# ============================================================================

class ZoneModel:
    """Loads models/zone_model.joblib + .json. Missing or incompatible = off."""

    def __init__(self, folder):
        self.folder = folder
        self.model = None
        self.count_model = None
        self.meta = None
        self.status = "no model yet - collect data on /collect, then run train_model.py"
        self.hold = {}            # zone -> currently shown as occupied (hysteresis)
        self.count_avg = None     # smoothed head-count probabilities

    def load(self):
        self.model, self.count_model, self.meta, self.hold, self.count_avg = None, None, None, {}, None
        meta_path = os.path.join(self.folder, "zone_model.json")
        model_path = os.path.join(self.folder, "zone_model.joblib")
        if not (os.path.exists(meta_path) and os.path.exists(model_path)):
            self.status = "no model yet - collect data on /collect, then run train_model.py"
            return False
        try:
            with open(meta_path) as f:
                meta = json.load(f)
            if meta.get("feature_version") != csi_features.FEATURE_VERSION:
                self.status = "model was trained on older features - re-run train_model.py"
                return False
            import joblib                       # comes with scikit-learn
            import sklearn
            if meta.get("sklearn_version") != sklearn.__version__:
                self.status = (f"model built with scikit-learn {meta.get('sklearn_version')}, "
                               f"this Pi has {sklearn.__version__} - re-run train_model.py here")
                return False
            bundle = joblib.load(model_path)
            if isinstance(bundle, dict):
                self.model, self.count_model = bundle["zones"], bundle.get("count")
            else:
                self.model = bundle
            self.meta = meta
            acc = meta.get("metrics", {}).get("zone_accuracy")
            self.status = (f"loaded ({meta.get('trained_at', '?')}, "
                           f"{meta.get('windows', '?')} training windows"
                           + (f", held-out zone accuracy {acc:.0%}" if acc is not None else "") + ")")
            return True
        except Exception as error:          # sklearn missing, corrupt file, ...
            self.model = None
            self.status = f"could not load model: {error}"
            return False

    @property
    def ready(self):
        return self.model is not None

    def link_order(self):
        return self.meta["links"] if self.meta else []

    def predict(self, feature_vector):
        """-> ({zone: probability occupied}, [p(0 people), p(1), p(2), p(3)] or None)"""
        zones = self.meta["zones"]
        X = feature_vector.reshape(1, -1)
        probs = self.model.predict_proba(X)
        out = {}
        for zone, p, classes in zip(zones, probs, self.model.classes_):
            classes = list(classes)
            out[zone] = float(p[0][classes.index(1)]) if 1 in classes else 0.0
        counts = None
        if self.count_model is not None:
            counts = [0.0] * (MAX_PEOPLE + 1)
            for c, p in zip(self.count_model.classes_, self.count_model.predict_proba(X)[0]):
                if int(c) <= MAX_PEOPLE:
                    counts[int(c)] = float(p)
        return out, counts

    def occupied(self, probs, counts=None):
        """
        Zones shown as occupied. With a head-count model: the N most likely
        zones, N = smoothed most-likely head count. Without: zones above a
        threshold. Either way with a little "stickiness" so they don't flicker.
        """
        if counts is not None:
            c = np.asarray(counts)
            self.count_avg = c if self.count_avg is None else 0.6 * self.count_avg + 0.4 * c
            n = int(np.argmax(self.count_avg))
            ranked = sorted(probs, key=lambda z: -(probs[z] + (0.08 if self.hold.get(z) else 0.0)))
            chosen = ranked[:n]
            self.hold = {z: (z in chosen) for z in probs}
            return chosen
        result = []
        for zone, p in probs.items():
            was = self.hold.get(zone, False)
            now = p >= ZONE_ON or (was and p >= ZONE_OFF)
            self.hold[zone] = now
            if now:
                result.append(zone)
        result.sort(key=lambda z: -probs[z])
        return result[:MAX_PEOPLE]

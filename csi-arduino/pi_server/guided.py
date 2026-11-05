"""
============================================================================
 guided.py  -  guided data collection for volunteers  (/collect page)
============================================================================
 The server runs a timed schedule. The /collect page (on a laptop, TV or
 phone in the room) shows each volunteer which floor zone to stand in,
 speaks it out loud, and counts down. Every CSI row logged meanwhile gets
 a label saying exactly who was where, so nobody types labels by hand:

   guided;sess=s20261104-2310;step=4;phase=hold;act=stand;occ=A2+C1;n=2

   phase  "move" (walking to the next zone - NOT used for training)
          "hold" (in position - used for training)
   act    "empty" (everyone outside the room), "stand" or "walk"
          ("walk" = keep moving around inside your own zone)
   occ    occupied zones, or "none"

 The first step is always an empty room: it calibrates the "empty"
 reference automatically. The last step is empty too (lets train_model.py
 check that the room didn't drift during the session).
============================================================================
"""

import json
import os
import random
import threading
import time
from datetime import datetime

VOLUNTEER_NAMES = ["Blue", "Orange", "Green"]     # match the 3D view colours


class GuidedSession:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.lock = threading.Lock()
        self.steps = []           # dicts: index phase act zones start end
        self.session_id = None
        self.config = None
        self.stopped_at = None

    # ------------------------------------------------------------------
    def start(self, zones, volunteers=1, hold_s=20, move_s=6, rounds=2,
              activities=("stand", "walk"), empty_s=30, now=None):
        volunteers = max(1, min(len(VOLUNTEER_NAMES), int(volunteers)))
        hold_s = max(5.0, float(hold_s))
        move_s = max(2.0, float(move_s))
        rounds = max(1, int(rounds))
        empty_s = max(10.0, float(empty_s))
        activities = [a for a in activities if a in ("stand", "walk")] or ["stand"]
        now = time.time() if now is None else now

        plan = [("empty", [])]
        nz = len(zones)
        stride = max(1, nz // volunteers)
        for _ in range(rounds):
            for act in activities:
                order = list(zones)
                random.shuffle(order)
                for k in range(nz):
                    # Volunteer j stands `stride` zones further along the shuffled
                    # list, so everyone visits every zone once per pass and
                    # never two people in one zone.
                    plan.append((act, [order[(k + j * stride) % nz] for j in range(volunteers)]))
        plan.append(("empty", []))

        steps, t = [], now + 3.0          # 3 s to read the screen before the first move
        for i, (act, zs) in enumerate(plan):
            hold = empty_s if act == "empty" else hold_s
            if i == len(plan) - 1 and act == "empty":
                hold = empty_s / 2
            steps.append({"index": i, "phase": "move", "act": act, "zones": zs,
                          "start": t, "end": t + move_s})
            t += move_s
            steps.append({"index": i, "phase": "hold", "act": act, "zones": zs,
                          "start": t, "end": t + hold})
            t += hold

        with self.lock:
            self.steps = steps
            self.session_id = datetime.now().strftime("s%Y%m%d-%H%M%S")
            self.config = {"volunteers": volunteers, "hold_s": hold_s, "move_s": move_s,
                           "rounds": rounds, "activities": activities, "empty_s": empty_s,
                           "zones": list(zones)}
            self.stopped_at = None
        self._save()
        return self.session_id

    def stop(self):
        with self.lock:
            if self.steps and self.stopped_at is None:
                self.stopped_at = time.time()
        self._save()

    def _save(self):
        with self.lock:
            if not self.session_id:
                return
            folder = os.path.join(self.data_dir, "sessions")
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, self.session_id + ".json"), "w") as f:
                json.dump({"session": self.session_id, "config": self.config,
                           "stopped_at": self.stopped_at, "steps": self.steps}, f, indent=1)

    # ------------------------------------------------------------------
    def _current(self, now):
        if not self.steps or self.stopped_at is not None:
            return None
        if now >= self.steps[-1]["end"]:
            return None
        for s in self.steps:
            if s["start"] <= now < s["end"]:
                return s
        return None                      # the 3 s lead-in before step 0

    def active(self, now=None):
        now = time.time() if now is None else now
        with self.lock:
            return (bool(self.steps) and self.stopped_at is None
                    and now < self.steps[-1]["end"])

    def label(self, now=None):
        """CSV label for this moment, or None when no session is running."""
        now = time.time() if now is None else now
        with self.lock:
            s = self._current(now)
            if s is None:
                if self.steps and self.stopped_at is None and now < self.steps[0]["start"]:
                    return f"guided;sess={self.session_id};step=-1;phase=move;act=empty;occ=none;n=0"
                return None
            occ = "+".join(s["zones"]) if s["zones"] else "none"
            return (f"guided;sess={self.session_id};step={s['index']};phase={s['phase']};"
                    f"act={s['act']};occ={occ};n={len(s['zones'])}")

    def state(self, now=None):
        """Everything the /collect page needs."""
        now = time.time() if now is None else now
        with self.lock:
            if not self.steps:
                return {"running": False, "session": None}
            s = self._current(now)
            last = self.steps[-1]
            running = self.stopped_at is None and now < last["end"]
            nxt = None
            if s is not None:
                later = [x for x in self.steps if x["start"] >= s["end"] and x["phase"] == "hold"]
                if later:
                    nxt = {"act": later[0]["act"], "zones": later[0]["zones"]}
            holds = [x for x in self.steps if x["phase"] == "hold"]
            done = len([x for x in holds if x["end"] <= now])
            return {
                "running": running,
                "session": self.session_id,
                "config": self.config,
                "volunteer_names": VOLUNTEER_NAMES[: self.config["volunteers"]],
                "lead_in": running and s is None and now < self.steps[0]["start"],
                "step": None if s is None else {
                    "index": s["index"], "phase": s["phase"], "act": s["act"],
                    "zones": s["zones"], "remaining": round(s["end"] - now, 1),
                    "duration": round(s["end"] - s["start"], 1)},
                "next": nxt,
                "progress": {"done": done, "total": len(holds)},
                "ends_in": round(max(0.0, last["end"] - now), 0),
                "stopped": self.stopped_at is not None,
            }

    def current_empty_hold(self, now=None):
        """(start, end) of the empty-room hold happening right now, else None."""
        now = time.time() if now is None else now
        with self.lock:
            s = self._current(now)
            if s and s["phase"] == "hold" and s["act"] == "empty":
                return s["start"], s["end"]
        return None


def parse_label(label):
    """'guided;sess=..;step=..' -> dict, or None for hand-typed labels."""
    if not label or not label.startswith("guided;"):
        return None
    out = {}
    for part in label.split(";")[1:]:
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    out["zones"] = [] if out.get("occ", "none") == "none" else out["occ"].split("+")
    return out

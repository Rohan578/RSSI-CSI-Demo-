"""
============================================================================
 sim_room.py  -  a simulated room with people walking around
============================================================================
 Pretends to be the receivers listed in room.json and sends CSI to
 server.py in the exact UDP format the real boards use. The CSI comes from
 a simple physical model of the room, so the 3D view, calibration, the
 benchmark and AI training can all be tested without hardware:

   - radio paths: direct, wall/floor/ceiling reflections, and fixed
     "furniture" scatterers  ->  a position-dependent fingerprint
   - each person reflects signal AND blocks the paths passing through them
   - breathing / swaying when standing, fast changes when walking
   - ESP32 quirks: automatic gain steps, integer RSSI, a bad first word,
     the odd 802.11g sample, lost packets
   - receivers from room.json:
       2.4 GHz receivers hear every transmitter (ESP-NOW)
       a 5 GHz receiver with "source": "access_point" hears the ROUTER
       (node 0, at "access_point" in room.json) - like the C5 pinging it
   - optional home-WiFi interference (--interference): bursts that hit
     every 2.4 GHz link of a receiver at once (other devices, Bluetooth,
     a microwave) - extra noise on some subcarriers, inflated RSSI and
     lost packets. 5 GHz is barely affected.

 It also tells the server where its fake people REALLY are (/api/sim/truth)
 so the 3D view can draw them and score itself.

 USAGE
   python sim_room.py                         # scripted: 0,1,2,3,1,0 people...
   python sim_room.py --people 2              # always 2 people wandering
   python sim_room.py --follow-collect        # act out guided sessions (/collect)
   python sim_room.py --interference          # add home-WiFi interference
   python sim_room.py --host 192.168.1.20     # send to the server on another PC

 The real room will behave differently (real walls, real bodies). This tests
 the SOFTWARE; it does not predict real accuracy.
============================================================================
"""

import argparse
import json
import math
import os
import random
import socket
import struct
import threading
import time
import urllib.request

import numpy as np

# Must match csi_packet.h and server.py
CSI_PACKET_MAGIC = 0xC51A
CSI_PACKET_VERSION = 2
BATCH_HEADER = struct.Struct("<HBBIB3x")
SAMPLE_HEADER = struct.Struct("<BbbBBBHII")
MAX_UDP_PAYLOAD = 1400

LIGHT_SPEED = 3e8
SUBCARRIERS = np.r_[-26:0, 1:27]                       # data subcarriers
BLOCK_POS = np.where(SUBCARRIERS > 0, SUBCARRIERS, 64 + SUBCARRIERS)   # driver order
BODY_RADIUS = 0.22
BODY_HEIGHT = 1.75
DEFAULT_SCRIPT = "0:25,1:50,2:50,3:40,1:40,2:40,0:20"


def channel_freq(channel):
    if channel <= 14:
        return (2407 + 5 * channel) * 1e6
    return (5000 + 5 * channel) * 1e6


def receiver_band(cfg):
    return "5" if str(cfg.get("band", "2.4")).startswith("5") else "2.4"


def receiver_source(cfg):
    return cfg.get("source", "access_point" if receiver_band(cfg) == "5" else "transmitters")


# ============================================================================
#  The room's radio physics
# ============================================================================

class RadioRoom:
    def __init__(self, room, channel, seed=7):
        self.room = room
        self.w, self.d, self.h = room["width"], room["depth"], room.get("height", 2.3)
        rng = np.random.default_rng(seed)
        self.freqs = channel_freq(channel) + SUBCARRIERS * 312.5e3
        self.rx = {int(k): np.array([v["x"], v["y"], v["z"]]) for k, v in room["receivers"].items()}
        self.tx = {int(k): np.array([v["x"], v["y"], v["z"]]) for k, v in room["transmitters"].items()}
        ap = room.get("access_point")
        if ap:                                          # node 0 = the router
            self.tx[0] = np.array([ap["x"], ap["y"], ap.get("z", 1.5)])
        # Fixed scatterers: furniture, pipes, the water heater...
        n = 9
        self.scatterers = np.column_stack([rng.uniform(0.3, self.w - 0.3, n),
                                           rng.uniform(0.3, self.d - 0.3, n),
                                           rng.uniform(0.2, 2.0, n)])
        self.scatter_gain = rng.uniform(0.25, 0.6, n)
        # (axis, plane position, reflection coefficient)
        self.planes = [(0, 0.0, 0.5), (0, self.w, 0.5), (1, 0.0, 0.5), (1, self.d, 0.5),
                       (2, 0.0, 0.35), (2, self.h, 0.3)]

    @staticmethod
    def _shadow(p, q, people):
        """How much of a straight path p->q gets through the people (0..1)."""
        f = 1.0
        dx, dy = q[0] - p[0], q[1] - p[1]
        l2 = dx * dx + dy * dy
        for (px, py, _pz) in people:
            if l2 < 1e-9:
                continue
            t = ((px - p[0]) * dx + (py - p[1]) * dy) / l2
            if t <= 0.02 or t >= 0.98:
                continue
            z = p[2] + t * (q[2] - p[2])
            if z > BODY_HEIGHT or z < 0:
                continue
            dist = math.hypot(px - (p[0] + t * dx), py - (p[1] + t * dy))
            f *= 1.0 - 0.8 * math.exp(-(dist / BODY_RADIUS) ** 2)
        return f

    def paths(self, tx, rx, people):
        """List of (length_m, amplitude) for every path tx -> rx."""
        out = []
        d = float(np.linalg.norm(tx - rx))
        out.append((d, self._shadow(tx, rx, people) / d))
        for axis, pos, gamma in self.planes:
            img = tx.copy()
            img[axis] = 2 * pos - tx[axis]
            length = float(np.linalg.norm(img - rx))
            t = (pos - img[axis]) / (rx[axis] - img[axis]) if rx[axis] != img[axis] else 0.5
            refl = img + t * (rx - img)
            f = self._shadow(tx, refl, people) * self._shadow(refl, rx, people)
            out.append((length, gamma * f / length))
        for s, g in zip(self.scatterers, self.scatter_gain):
            d1, d2 = float(np.linalg.norm(tx - s)), float(np.linalg.norm(s - rx))
            f = self._shadow(tx, s, people) * self._shadow(s, rx, people)
            out.append((d1 + d2, g * f / (d1 * d2)))
        for (px, py, pz) in people:
            body = np.array([px, py, pz])
            d1, d2 = float(np.linalg.norm(tx - body)), float(np.linalg.norm(body - rx))
            out.append((d1 + d2, 0.45 / (d1 * d2)))
        return out

    def channel(self, tx_id, rx_id, people):
        paths = self.paths(self.tx[tx_id], self.rx[rx_id], people)
        lengths = np.array([p[0] for p in paths])
        amps = np.array([p[1] for p in paths])
        phase = -2j * np.pi * np.outer(self.freqs, lengths) / LIGHT_SPEED
        return (np.exp(phase) * amps).sum(axis=1)          # complex, 52 subcarriers


# ============================================================================
#  Home-WiFi interference (2.4 GHz)
# ============================================================================

class Interference:
    """
    Bursts of other 2.4 GHz traffic. While a burst is on, every link of the
    receiver is affected at the same time - that "common mode" is what
    tells interference apart from a person, who changes only some links.
    """

    def __init__(self, mean_gap_s=8.0, rng=None):
        self.mean_gap_s = mean_gap_s
        self.rng = rng or random.Random(11)
        self.next_start = self.rng.expovariate(1.0 / mean_gap_s)
        self.end = -1.0
        self.band = None

    def active(self, t):
        if t >= self.next_start and t > self.end:
            self.end = t + self.rng.uniform(0.3, 2.0)
            lo = self.rng.randint(0, 40)
            self.band = (lo, lo + self.rng.randint(6, 12))   # affected subcarriers
            self.power = self.rng.uniform(0.6, 1.6)
            self.next_start = self.end + self.rng.expovariate(1.0 / self.mean_gap_s)
        return t <= self.end

    def apply(self, h, t):
        """-> (h, extra_rssi_db, dropped)"""
        if not self.active(t):
            return h, 0.0, False
        if self.rng.random() < 0.3:
            return h, 0.0, True                               # collision: packet lost
        if self.rng.random() < 0.6:                           # this packet overlapped
            lo, hi = self.band
            scale = float(np.mean(np.abs(h)))
            noise = (np.random.normal(0, 1, hi - lo) + 1j * np.random.normal(0, 1, hi - lo))
            h = h.copy()
            h[lo:hi] += noise * scale * self.power
            return h, self.rng.uniform(1.0, 6.0) * self.power, False
        return h, 0.0, False


# ============================================================================
#  People
# ============================================================================

class Person:
    def __init__(self, pid, x, y):
        self.id = pid
        self.x, self.y = x, y
        self.target = (x, y)
        self.speed = random.uniform(0.6, 1.0)
        self.pause_until = 0.0
        self.mode = "wander"            # wander | goto | stand | zone
        self.zone_box = None
        self.phase = random.uniform(0, 6.28)

    def torso(self, t):
        # Breathing + swaying: a few millimetres to a centimetre
        sway = 0.006 * math.sin(2 * math.pi * 0.27 * t + self.phase)
        return (self.x + sway, self.y + 0.4 * sway, 1.15)


def random_point(box, margin=0.35):
    x0, y0, x1, y1 = box
    return (random.uniform(x0 + margin, x1 - margin), random.uniform(y0 + margin, y1 - margin))


class Crowd:
    def __init__(self, room):
        self.room = room
        self.w, self.d = room["width"], room["depth"]
        self.people = []
        self.next_id = 1
        self.step_key = None

    def room_box(self):
        return (0.0, 0.0, self.w, self.d)

    def zone_box(self, name):
        cols, rows = room_zones(self.room)
        c = ord(name[0]) - ord("A")
        r = int(name[1:]) - 1
        zw, zd = self.w / cols, self.d / rows
        return (c * zw, r * zd, (c + 1) * zw, (r + 1) * zd)

    def set_count(self, n):
        while len(self.people) < n:
            x, y = random_point(self.room_box(), 0.6)
            p = Person(self.next_id, x, y)
            self.next_id += 1
            p.target = random_point(self.room_box(), 0.6)
            self.people.append(p)
        while len(self.people) > n:
            self.people.pop()

    def follow(self, step, now):
        """Act out the current guided-session step like a volunteer would."""
        key = (step["index"], step["act"])
        zones = step["zones"]
        self.set_count(len(zones))
        new_step = key != self.step_key
        self.step_key = key
        for p, zone in zip(self.people, zones):
            box = self.zone_box(zone)
            if new_step:
                p.target = random_point(box)
                p.zone_box = box
                p.speed = 1.0
            if step["phase"] == "move":
                p.mode = "goto"
            else:
                p.mode = "stand" if step["act"] == "stand" else "zone"
                if p.mode == "zone":
                    p.speed = 0.45

    def move(self, dt, now):
        for p in self.people:
            tx, ty = p.target
            dx, dy = tx - p.x, ty - p.y
            dist = math.hypot(dx, dy)
            if p.mode == "stand" and dist < 0.05:
                continue
            if now < p.pause_until:
                continue
            if dist < 0.05:
                if p.mode == "wander":
                    p.pause_until = now + random.uniform(1.0, 5.0)
                    p.target = random_point(self.room_box(), 0.6)
                    p.speed = random.uniform(0.5, 1.0)
                elif p.mode == "zone" and p.zone_box:
                    p.target = random_point(p.zone_box, 0.25)
                continue
            stepm = min(dist, p.speed * dt)
            p.x += dx / dist * stepm
            p.y += dy / dist * stepm

    def wander_all(self):
        self.step_key = None
        for p in self.people:
            if p.mode != "wander":
                p.mode = "wander"
                p.target = random_point(self.room_box(), 0.6)


def room_zones(room):
    z = room.get("zones", {})
    return int(z.get("cols", 3)), int(z.get("rows", 3))


# ============================================================================
#  One simulated receiver: ESP32 quirks + sample packing
# ============================================================================

class FakeReceiver:
    """
    Turns complex channels into the receiver's samples (dicts in the same
    shape server.parse_batch() returns) and, with a socket, UDP batches.
    lltf_only: send just the 128-byte LLTF block (what the new 2.4 GHz
    firmware does); otherwise 384 bytes (LLTF + HT-LTF + STBC block).
    """

    def __init__(self, receiver_id, channel, loss=0.01, lltf_only=True,
                 interference=None, host=None, port=5005):
        self.receiver_id = receiver_id
        self.channel = channel
        self.loss = loss
        self.lltf_only = lltf_only
        self.interference = interference
        self.addr = (host, port) if host else None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if host else None
        self.batch_seq = 0
        self.pending = []
        self.gain_db = {}
        self.seq = {}

    def _quantize(self, node, h, extra_rssi_db):
        power = float(np.mean(np.abs(h) ** 2))
        level_db = 10 * math.log10(max(power, 1e-12))
        # AGC: re-pick a gain in 1 dB steps only when the level drifts > 2 dB
        want = 20 * math.log10(31.0) - level_db
        g = self.gain_db.get(node)
        if g is None or abs(g - want) > 2.0:
            g = float(round(want))
            self.gain_db[node] = g
        scaled = h * (10 ** (g / 20))
        scaled = scaled * np.exp(1j * random.uniform(0, 2 * math.pi))   # random common phase
        noise = np.random.normal(0, 0.9, (2, h.size))
        imag = np.clip(np.round(scaled.imag + noise[0]), -128, 127)
        real = np.clip(np.round(scaled.real + noise[1]), -128, 127)
        base = -40 if self.channel <= 14 else -46                        # 5 GHz is weaker
        rssi = int(round(base + level_db + extra_rssi_db + random.gauss(0, 0.7)))
        return imag.astype(np.int8), real.astype(np.int8), max(-95, min(-20, rssi))

    def make_sample(self, node, h, now):
        """-> sample dict, or None if the packet was lost."""
        self.seq[node] = self.seq.get(node, 0) + 1
        if random.random() < self.loss:
            return None
        extra = 0.0
        if self.interference is not None:
            h, extra, dropped = self.interference.apply(h, now)
            if dropped:
                return None
        imag, real, rssi = self._quantize(node, h, extra)
        block = np.zeros(128, dtype=np.int8)
        block[2 * BLOCK_POS] = imag
        block[2 * BLOCK_POS + 1] = real
        legacy = node != 0 and random.random() < 0.02    # the odd 802.11g packet
        first_bad = random.random() < 0.05
        if first_bad:
            block[:4] = np.random.randint(-128, 127, 4)
        if legacy or self.lltf_only:
            csi = block
        else:
            ht = np.clip(block.astype(np.int16) + np.random.randint(-1, 2, 128), -128, 127).astype(np.int8)
            ht[:2] = 0
            csi = np.concatenate([block, ht, np.zeros(128, np.int8)])
        raw = csi.tobytes()
        return {"tx_node_id": node, "tx_seq": 0 if node == 0 else self.seq[node],
                "rx_timestamp_us": int(now * 1e6) & 0xFFFFFFFF, "rssi": rssi,
                "noise_floor": -93, "channel": self.channel, "rx_format": 0 if legacy else 1,
                "first_word_invalid": 1 if first_bad else 0,
                "csi": list(csi.astype(int)), "raw": raw}

    def send(self, s):
        if self.sock is None or s is None:
            return
        header = SAMPLE_HEADER.pack(s["tx_node_id"], s["rssi"], s["noise_floor"], s["channel"],
                                    s["rx_format"], s["first_word_invalid"], len(s["raw"]),
                                    s["tx_seq"], s["rx_timestamp_us"])
        data = header + s["raw"]
        size = BATCH_HEADER.size + sum(len(p) for p in self.pending) + len(data)
        if self.pending and size > MAX_UDP_PAYLOAD:
            self.flush()
        self.pending.append(data)

    def flush(self):
        if not self.pending or self.sock is None:
            return
        self.batch_seq += 1
        header = BATCH_HEADER.pack(CSI_PACKET_MAGIC, CSI_PACKET_VERSION, self.receiver_id,
                                   self.batch_seq, len(self.pending))
        self.sock.sendto(header + b"".join(self.pending), self.addr)
        self.pending = []


class SimReceivers:
    """All receivers from room.json, each with its band, channel and sources."""

    def __init__(self, room, channel_24=1, channel_5=36, seed=7, loss=0.01,
                 interference=False, host=None, port=5005, only=None):
        self.radio = {"2.4": RadioRoom(room, channel_24, seed), "5": RadioRoom(room, channel_5, seed)}
        self.units = []                         # (receiver_id, band, [node ids], FakeReceiver)
        tx_nodes = sorted(int(k) for k in room["transmitters"])
        for k, cfg in sorted(room["receivers"].items(), key=lambda kv: int(kv[0])):
            rid = int(k)
            if cfg.get("enabled", True) is False or (only is not None and rid not in only):
                continue
            band = receiver_band(cfg)
            source = receiver_source(cfg)
            nodes = [0] if source == "access_point" else tx_nodes
            if 0 in nodes and not room.get("access_point"):
                continue
            inter = Interference(rng=random.Random(100 + rid)) if (interference and band == "2.4") else None
            fr = FakeReceiver(rid, channel_24 if band == "2.4" else channel_5, loss=loss,
                              lltf_only=True, interference=inter, host=host, port=port)
            self.units.append((rid, band, nodes, fr))

    def tick(self, bodies, now):
        """-> list of (receiver_id, sample) for this instant."""
        out = []
        for rid, band, nodes, fr in self.units:
            radio = self.radio[band]
            for node in nodes:
                s = fr.make_sample(node, radio.channel(node, rid, bodies), now)
                if s is not None:
                    out.append((rid, s))
                    fr.send(s)
            fr.flush()
        return out

    def describe(self):
        return ", ".join(f"rx {rid} ({band} GHz, {'router' if nodes == [0] else f'{len(nodes)} transmitters'})"
                         for rid, band, nodes, _ in self.units)


# ============================================================================
#  Talking to the server (truth + guided session)
# ============================================================================

def http_json(url, body=None, timeout=1.0):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


class ServerLink(threading.Thread):
    def __init__(self, base_url, crowd, lock, follow):
        super().__init__(daemon=True)
        self.base = base_url
        self.crowd = crowd
        self.lock = lock
        self.follow = follow
        self.collect = None

    def run(self):
        while True:
            try:
                with self.lock:
                    truth = [{"id": p.id, "x": round(p.x, 3), "y": round(p.y, 3)} for p in self.crowd.people]
                http_json(self.base + "/api/sim/truth", {"people": truth})
                if self.follow:
                    self.collect = http_json(self.base + "/api/collect")
            except Exception:
                pass
            time.sleep(0.25)


# ============================================================================
#  Main loop
# ============================================================================

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Simulated room -> server.py")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--web-port", type=int, default=5000)
    ap.add_argument("--room", default=os.path.join(here, "room.json"))
    ap.add_argument("--channel", type=int, default=1, help="2.4 GHz channel")
    ap.add_argument("--channel-5", type=int, default=36, help="5 GHz channel")
    ap.add_argument("--rate", type=int, default=50, help="packets per second per transmitter")
    ap.add_argument("--loss", type=float, default=0.01)
    ap.add_argument("--interference", action="store_true", help="add home-WiFi interference on 2.4 GHz")
    ap.add_argument("--people", type=int, default=None, help="fixed number of wandering people")
    ap.add_argument("--script", default=DEFAULT_SCRIPT,
                    help="people:seconds,... repeated, e.g. '0:20,1:60,2:60'")
    ap.add_argument("--follow-collect", action="store_true",
                    help="act out the guided session running on /collect")
    ap.add_argument("--seed", type=int, default=7, help="room layout seed (furniture)")
    ap.add_argument("--no-truth", action="store_true", help="don't send true positions")
    args = ap.parse_args()

    with open(args.room) as f:
        room = json.load(f)
    receivers = SimReceivers(room, args.channel, args.channel_5, args.seed, args.loss,
                             args.interference, args.host, args.port)
    crowd = Crowd(room)
    lock = threading.Lock()
    link = ServerLink(f"http://{args.host}:{args.web_port}", crowd, lock, args.follow_collect)
    if not args.no_truth:
        link.start()

    script = [(int(a), float(b)) for a, b in (part.split(":") for part in args.script.split(","))]
    script_len = sum(s for _, s in script)
    print(f"Simulating {receivers.describe()}; sending to {args.host}:{args.port}"
          f"{' with 2.4 GHz interference' if args.interference else ''}. Ctrl+C to stop.")
    if args.people is None and not args.follow_collect:
        print(f"People script (repeats): {args.script}")

    period = 1.0 / args.rate
    start = time.time()
    next_t = start
    last = start
    last_print = 0
    while True:
        now = time.time()
        dt = now - last
        last = now
        with lock:
            col = link.collect if args.follow_collect else None
            if col and col.get("running") and col.get("step"):
                crowd.follow(col["step"], now)
            elif col and col.get("running") and col.get("lead_in"):
                crowd.set_count(0)
            else:
                crowd.wander_all()
                if args.people is not None:
                    crowd.set_count(args.people)
                else:
                    t = (now - start) % script_len
                    for n, secs in script:
                        if t < secs:
                            crowd.set_count(n)
                            break
                        t -= secs
            crowd.move(dt, now)
            bodies = [p.torso(now) for p in crowd.people]
            count = len(crowd.people)
        receivers.tick(bodies, now)
        if now - last_print > 10:
            last_print = now
            print(f"[SIM] {count} person(s) in the room")
        next_t += period
        time.sleep(max(0.0, next_t - time.time()))
        if time.time() - next_t > 1:
            next_t = time.time()


if __name__ == "__main__":
    main()

"""
============================================================================
 sim_room.py  -  a simulated basement with people walking around
============================================================================
 Pretends to be the ESP32-S3 receiver hearing the transmitters listed in
 room.json, and sends CSI to server.py in the exact same UDP format.
 Unlike fake_receiver.py, the CSI comes from a (simple) physical model of
 the room, so the 3D view, calibration and AI training can be tested
 end-to-end without hardware:

   - radio paths: direct, wall/floor/ceiling reflections, and some fixed
     "furniture" scatterers  ->  a realistic, position-dependent fingerprint
   - each person reflects signal AND blocks the paths passing through them
   - breathing / swaying when standing, fast changes when walking
   - the ESP32's quirks: automatic gain steps, 384-byte CSI (LLTF + HT-LTF
     + STBC block), the odd 128-byte 802.11g sample, a bad first word,
     ~1% lost packets

 It also tells the server where its fake people REALLY are (/api/sim/truth),
 so the 3D view can show the true positions as ghosts and score itself.

 USAGE
   python3 sim_room.py                         # scripted: 0,1,2,3,1,0 people...
   python3 sim_room.py --people 2              # always 2 people wandering
   python3 sim_room.py --follow-collect        # act out guided sessions (/collect)
   python3 sim_room.py --host 192.168.1.50     # send to the Pi from a laptop

 The real room will behave differently (real walls, real bodies) - this is
 for testing the software, not for predicting accuracy.
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
            # reflection point on the plane
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
#  ESP32 quirks + UDP packing
# ============================================================================

class FakeS3:
    def __init__(self, host, port, receiver_id, channel, loss):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.receiver_id = receiver_id
        self.channel = channel
        self.loss = loss
        self.batch_seq = 0
        self.pending = []
        self.gain_db = {}
        self.seq = {}

    def _quantize(self, node, h, now):
        power = float(np.mean(np.abs(h) ** 2))
        level_db = 10 * math.log10(max(power, 1e-12))
        # AGC: re-pick a gain in 1 dB steps only when the level drifts > 2 dB
        target = 31.0
        want = 20 * math.log10(target) - level_db
        g = self.gain_db.get(node)
        if g is None or abs(g - want) > 2.0:
            g = float(round(want))
            self.gain_db[node] = g
        scaled = h * (10 ** (g / 20)) * math.exp(0)
        scaled *= np.exp(1j * random.uniform(0, 2 * math.pi))       # random common phase
        noise = np.random.normal(0, 0.9, (2, h.size))
        imag = np.clip(np.round(scaled.imag + noise[0]), -128, 127)
        real = np.clip(np.round(scaled.real + noise[1]), -128, 127)
        rssi = int(round(-36 + level_db + random.gauss(0, 0.7)))      # 1 dB steps, like the ESP32
        return imag.astype(np.int8), real.astype(np.int8), max(-95, min(-20, rssi))

    def sample(self, node, h, now):
        self.seq[node] = self.seq.get(node, 0) + 1
        if random.random() < self.loss:
            return
        imag, real, rssi = self._quantize(node, h, now)
        block = np.zeros(128, dtype=np.int8)
        block[2 * BLOCK_POS] = imag
        block[2 * BLOCK_POS + 1] = real
        legacy = random.random() < 0.02                  # the odd 802.11g packet
        first_bad = random.random() < 0.05
        if first_bad:
            block[:4] = np.random.randint(-128, 127, 4)
        if legacy:
            csi = block
            rx_format = 0
        else:
            ht = np.clip(block.astype(np.int16) + np.random.randint(-1, 2, 128), -128, 127).astype(np.int8)
            ht[:2] = 0                                   # DC
            csi = np.concatenate([block, ht, np.zeros(128, np.int8)])
            rx_format = 1
        header = SAMPLE_HEADER.pack(node, rssi, -93, self.channel, rx_format, 1 if first_bad else 0,
                                    csi.size, self.seq[node], int(now * 1e6) & 0xFFFFFFFF)
        data = header + csi.tobytes()
        size = BATCH_HEADER.size + sum(len(p) for p in self.pending) + len(data)
        if self.pending and size > MAX_UDP_PAYLOAD:
            self.flush()
        self.pending.append(data)

    def flush(self):
        if not self.pending:
            return
        self.batch_seq += 1
        header = BATCH_HEADER.pack(CSI_PACKET_MAGIC, CSI_PACKET_VERSION, self.receiver_id,
                                   self.batch_seq, len(self.pending))
        self.sock.sendto(header + b"".join(self.pending), self.addr)
        self.pending = []


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
    ap.add_argument("--channel", type=int, default=11)
    ap.add_argument("--rate", type=int, default=50, help="packets per second per transmitter")
    ap.add_argument("--loss", type=float, default=0.01)
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
    radio = RadioRoom(room, args.channel, seed=args.seed)
    crowd = Crowd(room)
    s3 = FakeS3(args.host, args.port, 1, args.channel, args.loss)
    lock = threading.Lock()
    link = ServerLink(f"http://{args.host}:{args.web_port}", crowd, lock, args.follow_collect)
    if not args.no_truth:
        link.start()

    script = [(int(a), float(b)) for a, b in (part.split(":") for part in args.script.split(","))]
    script_len = sum(s for _, s in script)
    rx_id = sorted(radio.rx)[0]
    nodes = sorted(radio.tx)
    print(f"Simulating {len(nodes)} transmitters -> receiver {rx_id} on channel {args.channel}, "
          f"sending to {args.host}:{args.port}. Ctrl+C to stop.")
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
        for node in nodes:
            s3.sample(node, radio.channel(node, rx_id, bodies), now)
        s3.flush()
        if now - last_print > 10:
            last_print = now
            print(f"[SIM] {count} person(s) in the room")
        next_t += period
        time.sleep(max(0.0, next_t - time.time()))
        if time.time() - next_t > 1:
            next_t = time.time()


if __name__ == "__main__":
    main()

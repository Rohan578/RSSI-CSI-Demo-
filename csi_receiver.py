#!/usr/bin/env python3
"""
CSI receiver - listens for ADR-018 CSI frames sent over UDP by the ESP32-S3
(esp32_CSI_sketch.ino), prints live per-node statistics, and can log every
frame to a CSV file.

Usage:
    python3 csi_receiver.py                       # listen on 0.0.0.0:5005
    python3 csi_receiver.py --csv logs/run1.csv   # also log frames
    python3 csi_receiver.py --dump                # print every frame

Standard library only (Python 3.8+).

ADR-018 frame, little-endian:
    off  size  field
      0    4   magic        0xC5110001
      4    1   node_id
      5    1   n_antennas
      6    2   n_subcarriers
      8    4   freq_mhz
     12    4   sequence
     16    1   rssi         (int8, dBm)
     17    1   noise_floor  (int8, dBm)
     18    2   reserved
     20    ..  CSI payload: n_antennas * n_subcarriers * 2 int8 values,
                 per subcarrier as (imaginary, real) - the ESP-IDF order.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

MAGIC = 0xC5110001
HEADER_FMT = "<IBBHIIbbBB"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 20

CSV_COLUMNS = [
    "recv_time", "src_ip", "node_id", "seq", "freq_mhz", "rssi", "noise",
    "n_antennas", "n_subcarriers", "truncated", "iq",
]


# ---------------------------------------------------------------------------
#  Parsing
# ---------------------------------------------------------------------------

@dataclass
class CsiFrame:
    node_id: int
    n_antennas: int
    n_subcarriers: int      # as declared in the header
    freq_mhz: int
    seq: int
    rssi: int
    noise: int
    iq: Tuple[int, ...]     # raw int8 values: imag0, real0, imag1, real1, ...
    truncated: bool         # payload shorter than the header promised

    def amplitudes(self) -> List[float]:
        """Per-subcarrier amplitude, sqrt(imag^2 + real^2)."""
        return [math.hypot(self.iq[i], self.iq[i + 1])
                for i in range(0, len(self.iq) - 1, 2)]

    def phases(self) -> List[float]:
        """Per-subcarrier raw phase in radians (atan2(imag, real))."""
        return [math.atan2(self.iq[i], self.iq[i + 1])
                for i in range(0, len(self.iq) - 1, 2)]


def parse_packet(data: bytes) -> CsiFrame:
    """Decode one UDP datagram. Raises ValueError if it is not a valid frame."""
    if len(data) < HEADER_SIZE:
        raise ValueError(f"short packet ({len(data)} bytes)")

    (magic, node_id, n_ant, n_sc, freq, seq,
     rssi, noise, _r1, _r2) = struct.unpack_from(HEADER_FMT, data)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08X}")

    payload = data[HEADER_SIZE:]
    expected = n_ant * n_sc * 2
    # The sketch caps the payload to its buffer, so it can be shorter than the
    # header says. Keep whatever whole (imag, real) pairs actually arrived.
    usable = min(len(payload), expected) & ~1
    iq = struct.unpack(f"<{usable}b", payload[:usable])

    return CsiFrame(node_id, n_ant, n_sc, freq, seq, rssi, noise,
                    iq, truncated=usable < expected)


# ---------------------------------------------------------------------------
#  Per-node statistics
# ---------------------------------------------------------------------------

class NodeStats:
    """Rolling stats for one node_id; counters reset every report window."""

    def __init__(self) -> None:
        self.last_seq: Optional[int] = None
        self.total = 0
        self.total_lost = 0
        self.resets = 0
        self._reset_window()

    def _reset_window(self) -> None:
        self.w_count = 0
        self.w_lost = 0
        self.w_rssi = 0
        self.w_amp = 0.0
        self.w_nsc = 0

    def add(self, f: CsiFrame) -> None:
        if self.last_seq is not None:
            if f.seq > self.last_seq + 1:
                lost = f.seq - self.last_seq - 1
                self.w_lost += lost
                self.total_lost += lost
            elif f.seq <= self.last_seq:
                self.resets += 1        # node rebooted or packet reordered
        self.last_seq = f.seq
        self.total += 1
        self.w_count += 1
        self.w_rssi += f.rssi
        self.w_nsc = len(f.iq) // 2
        amps = f.amplitudes()
        if amps:
            self.w_amp += sum(amps) / len(amps)

    def report(self, node_id: int, elapsed: float) -> Optional[str]:
        if self.w_count == 0:
            return None
        sent = self.w_count + self.w_lost
        line = (f"[{time.strftime('%H:%M:%S')}] node {node_id} | "
                f"{self.w_count / elapsed:6.1f} frames/s | "
                f"loss {100.0 * self.w_lost / sent:4.1f}% | "
                f"rssi {self.w_rssi / self.w_count:6.1f} dBm | "
                f"{self.w_nsc} subcarriers | "
                f"mean amp {self.w_amp / self.w_count:5.1f}")
        self._reset_window()
        return line


# ---------------------------------------------------------------------------
#  Main loop
# ---------------------------------------------------------------------------

def open_csv(path: str):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    is_new = not os.path.exists(path) or os.path.getsize(path) == 0
    fh = open(path, "a", newline="")
    writer = csv.writer(fh)
    if is_new:
        writer.writerow(CSV_COLUMNS)
    return fh, writer


def main() -> int:
    ap = argparse.ArgumentParser(description="Receive ESP32-S3 CSI frames over UDP.")
    ap.add_argument("--host", default="0.0.0.0", help="address to bind (default: all interfaces)")
    ap.add_argument("--port", type=int, default=5005, help="UDP port (default: 5005)")
    ap.add_argument("--csv", metavar="PATH", help="append every frame to this CSV file")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between stats lines (default: 1)")
    ap.add_argument("--dump", action="store_true", help="print every frame")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    try:
        sock.bind((args.host, args.port))
    except OSError as e:
        print(f"Cannot bind {args.host}:{args.port}: {e}", file=sys.stderr)
        return 1
    sock.settimeout(0.25)   # lets the loop print stats and notice Ctrl-C

    csv_fh = csv_writer = None
    if args.csv:
        csv_fh, csv_writer = open_csv(args.csv)
        print(f"Logging frames to {args.csv}")

    print(f"Listening for CSI frames on udp://{args.host}:{args.port}  (Ctrl-C to stop)")

    stats: Dict[int, NodeStats] = {}
    bad_packets = 0
    window_start = time.monotonic()

    try:
        while True:
            try:
                data, (src_ip, _src_port) = sock.recvfrom(2048)
            except socket.timeout:
                data = None

            if data is not None:
                try:
                    frame = parse_packet(data)
                except ValueError as e:
                    bad_packets += 1
                    print(f"[warn] dropped packet from {src_ip}: {e}", file=sys.stderr)
                else:
                    stats.setdefault(frame.node_id, NodeStats()).add(frame)
                    if args.dump:
                        amps = frame.amplitudes()
                        print(f"node {frame.node_id} seq {frame.seq} rssi {frame.rssi} "
                              f"noise {frame.noise} ch {frame.freq_mhz}MHz "
                              f"sc {len(amps)}{' (truncated)' if frame.truncated else ''} "
                              f"amp[:8] {[round(a, 1) for a in amps[:8]]}")
                    if csv_writer:
                        csv_writer.writerow([
                            f"{time.time():.6f}", src_ip, frame.node_id, frame.seq,
                            frame.freq_mhz, frame.rssi, frame.noise, frame.n_antennas,
                            frame.n_subcarriers, int(frame.truncated),
                            " ".join(map(str, frame.iq)),
                        ])

            now = time.monotonic()
            if now - window_start >= args.interval:
                elapsed = now - window_start
                window_start = now
                for node_id, st in sorted(stats.items()):
                    line = st.report(node_id, elapsed)
                    if line:
                        print(line)
                if csv_fh:
                    csv_fh.flush()
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        if csv_fh:
            csv_fh.close()

    print("\nSummary")
    if not stats:
        print("  no valid frames received")
    for node_id, st in sorted(stats.items()):
        print(f"  node {node_id}: {st.total} frames, {st.total_lost} lost by sequence gap, "
              f"{st.resets} sequence resets")
    if bad_packets:
        print(f"  {bad_packets} invalid packets dropped")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""
============================================================================
 Fake receiver  -  test the Pi server without any ESP32 hardware
============================================================================
 Pretends to be a receiver (S3 or C5): sends made-up CSI from several
 senders, in exactly the same UDP format (csi_packet.h, v2).
 Node 0 = the access point (what the C5 measures on 5 GHz).
 Useful for testing the server, dashboard and CSV logging, or for
 teammates working on the AI side before the hardware is ready.

 USAGE
   python3 fake_receiver.py                        # nodes 1,2,3 -> localhost
   python3 fake_receiver.py --nodes 1 2 20 21      # pick node IDs
   python3 fake_receiver.py --host 192.168.1.50    # send to the Pi from a laptop
   python3 fake_receiver.py --motion-every 10      # still/moving every 10 s

   Pretend to be the S3 (2.4 GHz) and the C5 (5 GHz) at the same time:
   python3 fake_receiver.py --receiver-id 1 --channel 6  --nodes 1 2 20 &
   python3 fake_receiver.py --receiver-id 2 --channel 36 --nodes 0
============================================================================
"""

import argparse
import math
import random
import socket
import struct
import time

# Must match csi_receiver_s3/csi_packet.h and server.py
CSI_PACKET_MAGIC = 0xC51A
CSI_PACKET_VERSION = 2
BATCH_HEADER = struct.Struct("<HBBIB3x")
SAMPLE_HEADER = struct.Struct("<BbbBBBHII")

SUBCARRIERS = 128            # 256 CSI bytes per sample
MAX_UDP_PAYLOAD = 1400


def fake_csi(t, moving, node_id, five_ghz):
    """Smooth amplitude 'shape' plus noise; much more wobble when 'moving'.
    5 GHz is made a little more sensitive to movement, as it tends to be."""
    values = []
    wobble = (0.45 if five_ghz else 0.35) if moving else 0.03
    for k in range(SUBCARRIERS):
        if k < 6 or 59 <= k <= 69 or k > 122:          # Empty guard subcarriers
            values += [0, 0]
            continue
        base = 18 + 8 * math.sin(k / 9 + node_id)
        amp = base * (1 + wobble * math.sin(t * 7 + k / 5 + node_id) + random.gauss(0, wobble / 2))
        phase = k / 4 + t
        imag = max(-128, min(127, round(amp * math.sin(phase))))
        real = max(-128, min(127, round(amp * math.cos(phase))))
        values += [imag, real]
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5005)
    parser.add_argument("--receiver-id", type=int, default=1)
    parser.add_argument("--nodes", type=int, nargs="+", default=[1, 2, 3],
                        help="sender IDs; 0 = access point")
    parser.add_argument("--channel", type=int, default=6, help="1-13 = 2.4 GHz, 36+ = 5 GHz")
    parser.add_argument("--rate", type=int, default=50, help="packets per second per node")
    parser.add_argument("--loss", type=float, default=0.01, help="fraction of packets to drop")
    parser.add_argument("--motion-every", type=float, default=8, help="seconds between still/moving")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx_seq = {n: 0 for n in args.nodes}
    batch_seq = 0
    pending = []
    start = time.time()
    print(f"Fake receiver {args.receiver_id} on channel {args.channel}: nodes {args.nodes} "
          f"-> {args.host}:{args.port}. Ctrl+C to stop.")

    def flush():
        nonlocal batch_seq, pending
        if not pending:
            return
        batch_seq += 1
        header = BATCH_HEADER.pack(CSI_PACKET_MAGIC, CSI_PACKET_VERSION, args.receiver_id,
                                   batch_seq, len(pending))
        sock.sendto(header + b"".join(pending), (args.host, args.port))
        pending = []

    while True:
        t = time.time() - start
        moving = int(t // args.motion_every) % 2 == 1
        for node in args.nodes:
            tx_seq[node] += 1
            if random.random() < args.loss:            # Simulate a lost packet
                continue
            five_ghz = args.channel >= 32
            csi = fake_csi(t, moving, node, five_ghz)
            rssi = -45 - node % 20 - (8 if five_ghz else 0)     # 5 GHz is weaker
            seq = 0 if node == 0 else tx_seq[node]              # AP has no counter
            header = SAMPLE_HEADER.pack(node, rssi, -95, args.channel, 1, 0, len(csi),
                                        seq, int(t * 1e6) & 0xFFFFFFFF)
            sample = header + struct.pack(f"<{len(csi)}b", *csi)
            size = BATCH_HEADER.size + sum(len(p) for p in pending) + len(sample)
            if pending and size > MAX_UDP_PAYLOAD:
                flush()
            pending.append(sample)
        flush()
        time.sleep(1 / args.rate)


if __name__ == "__main__":
    main()

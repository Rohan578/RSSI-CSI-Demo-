"""
============================================================================
 Band comparison  -  2.4 GHz vs 5 GHz, from a recorded CSV
============================================================================

 WHAT THIS DOES
   Reads a CSV that server.py recorded, cuts it into short time windows
   (1 second by default), and for every window works out how much
   movement each band saw. Then it compares the bands:

     both bands see movement        -> "movement"     (bands agree)
     neither band sees movement     -> "still"        (bands agree)
     only 2.4 GHz sees movement     -> "2.4 only"     probably INTERFERENCE
                                                       (microwave, Bluetooth,
                                                       other WiFi) - not a person
     only 5 GHz sees movement       -> "5 only"       probably a SMALL movement
                                                       that 2.4 GHz is too
                                                       coarse to notice

   It prints a summary and saves one row per window to a new CSV, with a
   "use_for_training" column: windows flagged "2.4 only" are marked 0 so
   you can leave them out (or use only their 5 GHz data) when training.

 HOW THE TWO RECEIVERS ARE LINED UP
   The S3 and C5 have separate clocks, so samples are matched by "pi_time":
   the moment the server received them. Good to within a few milliseconds.

 USAGE
   python3 compare_bands.py                       # newest file in data/
   python3 compare_bands.py data/csi_20261104_170000.csv
   python3 compare_bands.py --window 2 --threshold 6

 THE THRESHOLD
   "Movement" means a motion score above --threshold (default 5).
   Check the dashboard with the room empty and still, then pick a value a
   bit above what you see. 2.4 and 5 GHz can use different thresholds:
   --threshold-24 and --threshold-5.
============================================================================
"""

import argparse
import csv
import glob
import math
import os
import sys
from collections import defaultdict


# ============================================================================
#  SECTION 1: SETTINGS (defaults - can be changed on the command line)
# ============================================================================

DEFAULT_WINDOW_S = 1.0       # Length of each comparison window, seconds
DEFAULT_THRESHOLD = 5.0      # Motion score that counts as "movement"
MIN_SAMPLES_PER_LINK = 10    # Ignore a link in a window with fewer samples

csv.field_size_limit(10_000_000)   # CSI columns are long


# ============================================================================
#  SECTION 2: CSI MATH  (same as server.py)
# ============================================================================

def csi_amplitudes(csi_text):
    """'imag real imag real ...' -> one amplitude per subcarrier."""
    v = [int(x) for x in csi_text.split()]
    return [math.hypot(v[i + 1], v[i]) for i in range(0, len(v) - 1, 2)]


class WindowStats:
    """
    Running totals for one link in one time window. Stores sums instead of
    every sample, so even huge CSV files use little memory.
    """

    def __init__(self, n_subcarriers):
        self.n = 0
        self.sums = [0.0] * n_subcarriers
        self.squares = [0.0] * n_subcarriers

    def add(self, amps):
        if len(amps) != len(self.sums):
            return                              # Different packet type - skip
        self.n += 1
        for k, a in enumerate(amps):
            self.sums[k] += a
            self.squares[k] += a * a

    def motion(self):
        """
        Same idea as the dashboard's motion score: how much each subcarrier
        wobbles relative to its average, averaged and scaled to ~0-100.
        """
        if self.n < MIN_SAMPLES_PER_LINK:
            return None
        ratios = []
        for s, sq in zip(self.sums, self.squares):
            mean = s / self.n
            if mean < 1.0:                      # Empty guard subcarrier
                continue
            var = max(sq / self.n - mean * mean, 0.0)
            ratios.append(math.sqrt(var) / mean)
        return 100.0 * sum(ratios) / len(ratios) if ratios else None


# ============================================================================
#  SECTION 3: READING THE CSV INTO WINDOWS
# ============================================================================

def load_windows(path, window_s):
    """
    Returns:
      windows[w][link] = WindowStats     (w = window number from the start)
      labels[w]        = most common label in that window
    where link = (receiver_id, tx_node_id, band).
    """
    windows = defaultdict(dict)
    label_counts = defaultdict(lambda: defaultdict(int))
    start = None

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        needed = {"pi_time", "label", "receiver_id", "tx_node_id", "band", "csi"}
        if not needed.issubset(reader.fieldnames or []):
            sys.exit(f"{path} is missing columns {needed - set(reader.fieldnames or [])}. "
                     "Was it recorded by the dual-band server.py?")
        for row in reader:
            t = float(row["pi_time"])
            if start is None:
                start = t
            w = int((t - start) // window_s)
            link = (int(row["receiver_id"]), int(row["tx_node_id"]), row["band"])
            amps = csi_amplitudes(row["csi"])
            stats = windows[w].get(link)
            if stats is None:
                stats = windows[w][link] = WindowStats(len(amps))
            stats.add(amps)
            label_counts[w][row["label"]] += 1

    labels = {w: max(c, key=c.get) for w, c in label_counts.items()}
    return windows, labels, start


# ============================================================================
#  SECTION 4: COMPARING THE BANDS IN EACH WINDOW
#  >>> EDIT classify() to change how disagreements are judged.
# ============================================================================

def band_motion(links_in_window, band):
    """Average motion over every link on one band in one window."""
    scores = [s.motion() for (rx, tx, b), s in links_in_window.items() if b == band]
    scores = [m for m in scores if m is not None]
    return (sum(scores) / len(scores), len(scores)) if scores else (None, 0)


def classify(m24, m5, th24, th5):
    if m24 is None or m5 is None:
        return "one band missing"
    moving24, moving5 = m24 > th24, m5 > th5
    if moving24 and moving5:
        return "movement"
    if not moving24 and not moving5:
        return "still"
    if moving24:
        return "2.4 only"        # Probably interference on 2.4 GHz
    return "5 only"              # Probably a small movement


# ============================================================================
#  SECTION 5: MAIN
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="Compare 2.4 GHz and 5 GHz CSI")
    parser.add_argument("csv_file", nargs="?", help="CSV from server.py (default: newest in data/)")
    parser.add_argument("--window", type=float, default=DEFAULT_WINDOW_S, help="window length, seconds")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    parser.add_argument("--threshold-24", type=float, help="2.4 GHz threshold (default: --threshold)")
    parser.add_argument("--threshold-5", type=float, help="5 GHz threshold (default: --threshold)")
    args = parser.parse_args()

    path = args.csv_file
    if path is None:
        files = sorted(glob.glob(os.path.join("data", "csi_*.csv")))
        files = [f for f in files if not f.endswith("_bands.csv")]
        if not files:
            sys.exit("No CSV files in data/. Record some data with server.py first.")
        path = files[-1]
    th24 = args.threshold_24 if args.threshold_24 is not None else args.threshold
    th5 = args.threshold_5 if args.threshold_5 is not None else args.threshold

    print(f"Reading {path} ...")
    windows, labels, start = load_windows(path, args.window)
    if not windows:
        sys.exit("The file has no samples.")

    # --- Compare every window and write the results ---
    out_path = path[:-4] + "_bands.csv"
    counts = defaultdict(int)
    per_label = defaultdict(lambda: {"2.4 GHz": [], "5 GHz": []})
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["window", "start_time", "label", "links_24", "motion_24",
                         "links_5", "motion_5", "verdict", "use_for_training"])
        for w in sorted(windows):
            m24, n24 = band_motion(windows[w], "2.4 GHz")
            m5, n5 = band_motion(windows[w], "5 GHz")
            verdict = classify(m24, m5, th24, th5)
            counts[verdict] += 1
            if m24 is not None:
                per_label[labels[w]]["2.4 GHz"].append(m24)
            if m5 is not None:
                per_label[labels[w]]["5 GHz"].append(m5)
            writer.writerow([w, f"{start + w * args.window:.3f}", labels[w],
                             n24, "" if m24 is None else round(m24, 2),
                             n5, "" if m5 is None else round(m5, 2),
                             verdict, 0 if verdict == "2.4 only" else 1])

    # --- Summary ---
    total = sum(counts.values())
    print(f"\n{total} windows of {args.window:g} s  (thresholds: 2.4 GHz {th24:g}, 5 GHz {th5:g})\n")
    print("  Verdict            Windows   Share")
    for verdict in ("still", "movement", "2.4 only", "5 only", "one band missing"):
        if counts[verdict]:
            print(f"  {verdict:<18} {counts[verdict]:>7}   {100 * counts[verdict] / total:5.1f}%")

    print("\n  Average motion score per label (higher = band reacts more):")
    print(f"  {'Label':<22} {'2.4 GHz':>8} {'5 GHz':>8}")
    for label in sorted(per_label):
        cols = []
        for band in ("2.4 GHz", "5 GHz"):
            v = per_label[label][band]
            cols.append(f"{sum(v) / len(v):8.2f}" if v else f"{'-':>8}")
        print(f"  {label:<22} {cols[0]} {cols[1]}")

    if counts["2.4 only"]:
        print(f"\n  {counts['2.4 only']} window(s) look like 2.4 GHz interference. They are marked")
        print("  use_for_training = 0 in the output file.")
    print(f"\nSaved window-by-window results to {out_path}")


if __name__ == "__main__":
    main()

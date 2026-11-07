"""
============================================================================
 csi_features.py  -  raw ESP32 CSI  ->  numbers the localizer and AI use
============================================================================
 Shared by server.py (live) and train_model.py (offline), so training and
 live use EXACTLY the same features. If you change anything here, bump
 FEATURE_VERSION: the server then refuses to load models trained on the
 old features (and tells you to re-run train_model.py).

 THE STEPS, per CSI sample
   1. Take the 802.11 "LLTF" block (the first 128 bytes). Every packet type
      has it, so the features don't change if a packet arrives as 802.11g
      instead of 802.11n, or if csi_len differs (128 / 256 / 384 bytes).
   2. Keep the 52 data subcarriers (-26..-1, +1..+26). Drop DC and guards.
   3. Amplitude = sqrt(real^2 + imag^2).
   4. Divide by the sample's own mean amplitude. The ESP32's automatic gain
      control (AGC) jumps in steps, which scales the whole CSI vector up or
      down - without this, a gain step looks like a person walking in.
      Signal strength is kept separately as RSSI.

 PER WINDOW (1 second of samples from one link)
   motion     how much the subcarriers wobble (people moving)
   deviation  how far the average shape is from the EMPTY-room shape
              (people standing still - the wobble alone misses them)
   rssi change / rssi wobble   the LOCAL cue: a body on or near the direct
              path changes that link's power by a few dB. Shape changes are
              NOT local - in a small room a person anywhere changes every
              link's reflections - so the geometry estimate uses RSSI, while
              shape + wobble say "someone is here" and feed the AI model.
   plus 13 grouped "shape change" values and 13 grouped "wobble" values
   - used by the AI model.
============================================================================
"""

import math

import numpy as np

FEATURE_VERSION = 2          # 2: Hampel-filtered windows (interference-hit packets)

N_SC = 52                    # data subcarriers kept per sample
N_GROUPS = 13                # 52 subcarriers -> 13 groups of 4 neighbours
LINK_FEATURES = 2 * N_GROUPS + 4   # per link: shape(13) wobble(13) dev motion drssi rssi_std
MIN_WINDOW_SAMPLES = 12      # fewer samples than this in a window = link "stale"

# In each 64-subcarrier block the ESP32 / S3 order subcarriers 0..31, then -32..-1.
# Data subcarriers for 20 MHz: -26..-1 (positions 38..63) and +1..+26 (1..26).
SC_POSITIONS = np.r_[38:64, 1:27]
_PLUS_ONE = 26               # index of subcarrier +1 in SC_POSITIONS order

# Byte layouts the server can read. "esp32" is the documented S3 / ESP32
# order above. "linear" is -32..+31 in a row, which some newer chips (C5/C6)
# may use. LinkState in server.py checks the guard subcarriers of the first
# samples of every link and picks the layout that fits (see layout_ratio).
LAYOUTS = {
    "esp32":  {"data": SC_POSITIONS,         "guard": np.r_[29:36]},
    "linear": {"data": np.r_[6:32, 33:59],   "guard": np.r_[0:5, 60:64]},
}


def lltf_amplitudes(csi, first_word_invalid=False, layout="esp32"):
    """
    csi: the raw signed bytes [imag, real, imag, real, ...] (bytes or int8 array).
    Returns float32[52] amplitudes in subcarrier order -26..-1, +1..+26,
    or None if the sample is too short. layout: a key of LAYOUTS.
    """
    if isinstance(csi, (bytes, bytearray, memoryview)):
        v = np.frombuffer(csi, dtype=np.int8)
    else:
        v = np.asarray(csi, dtype=np.int8)
    if v.size < 128:
        return None
    block = v[:128].astype(np.float32)
    amp = np.hypot(block[1::2], block[0::2])[LAYOUTS[layout]["data"]]
    if first_word_invalid and layout == "esp32":
        # The first 4 bytes (subcarriers 0 and +1) are unreliable on this
        # packet: borrow the neighbour's value.
        amp[_PLUS_ONE] = amp[_PLUS_ONE + 1]
    return amp


LAYOUT_MAX_RATIO = 0.25       # guard energy above this = the bytes aren't laid out as assumed


def layout_ratio(csi, layout="esp32"):
    """
    Self-check for the byte layout everything above assumes. In the LLTF
    block the guard subcarriers carry no signal, so their amplitude should
    be ~0 compared with the data subcarriers. A large ratio means this
    receiver sends CSI in a different order (other chip, firmware or CSI
    settings) and every feature would be garbage. None = can't tell.
    """
    if isinstance(csi, (bytes, bytearray, memoryview)):
        v = np.frombuffer(csi, dtype=np.int8)
    else:
        v = np.asarray(csi, dtype=np.int8)
    if v.size < 128:
        return None
    block = v[:128].astype(np.float32)
    amp = np.hypot(block[1::2], block[0::2])
    data = float(amp[LAYOUTS[layout]["data"]].mean())
    if data < 0.5:
        return None
    return float(amp[LAYOUTS[layout]["guard"]].mean()) / data


def choose_layout(samples, n=200):
    """
    Pick the byte layout (key of LAYOUTS) that fits a link's first samples:
    the one whose guard subcarriers are empty. Same rule as server.py's
    live check, so training reads every link exactly like the live server.
    Returns (layout, ok).
    """
    med = {}
    for name in LAYOUTS:
        r = [x for x in (layout_ratio(c, name) for c in samples[:n]) if x is not None]
        if r:
            med[name] = sorted(r)[len(r) // 2]
    for name in ("esp32", "linear"):
        if med.get(name, 9) <= LAYOUT_MAX_RATIO:
            return name, True
    return "esp32", False


def normalize(amp):
    """Divide by the mean amplitude (removes AGC gain steps). None if ~empty."""
    if amp is None:
        return None
    m = float(amp.mean())
    if m < 0.5:
        return None
    return (amp / m).astype(np.float32)


class LinkBuffer:
    """Ring buffer of recent normalized amplitude vectors for one link."""

    def __init__(self, capacity=1500):          # 30 s at 50 Hz
        self.capacity = capacity
        self.t = np.zeros(capacity, dtype=np.float64)
        self.a = np.zeros((capacity, N_SC), dtype=np.float32)
        self.rssi = np.zeros(capacity, dtype=np.float32)
        self.count = 0          # total ever added
        self.pos = 0            # next write slot

    def add(self, t, amp_norm, rssi):
        self.t[self.pos] = t
        self.a[self.pos] = amp_norm
        self.rssi[self.pos] = rssi
        self.pos = (self.pos + 1) % self.capacity
        self.count += 1

    def since(self, t_start):
        """Copies of (t, a, rssi) for all samples newer than t_start, oldest first."""
        n = min(self.count, self.capacity)
        if n == 0:
            return np.zeros(0), np.zeros((0, N_SC), np.float32), np.zeros(0, np.float32)
        idx = (np.arange(self.pos - n, self.pos)) % self.capacity
        t = self.t[idx]
        keep = t > t_start
        idx = idx[keep]
        return self.t[idx].copy(), self.a[idx].copy(), self.rssi[idx].copy()


# ----------------------------------------------------------------------------
#  Window statistics
# ----------------------------------------------------------------------------

def _group(x):
    return x.reshape(N_GROUPS, -1).mean(axis=1)


HAMPEL_K = 3.0               # Robust filter: values this many MADs from the median are outliers


def hampel(a):
    """
    Replace outlier values with the window's median, per subcarrier.
    On a busy home network some packets overlap with other devices' traffic
    and come out with a few subcarriers badly distorted. One such packet
    looks like a big "wobble"; a person moving changes the CSI smoothly
    from packet to packet and survives this filter.
    """
    if len(a) < 5:
        return a
    med = np.median(a, axis=0)
    mad = 1.4826 * np.median(np.abs(a - med), axis=0)
    bad = np.abs(a - med) > HAMPEL_K * np.maximum(mad, 0.02)
    if not bad.any():
        return a
    return np.where(bad, med, a)


def rssi_level(rssi):
    """(level, wobble) of a window's RSSI: mean and standard deviation.
    (A median / trimmed std was tried: it hid real movement and lost more
    detections than it saved false alarms - see benchmark.py.)"""
    r = np.asarray(rssi, dtype=np.float64)
    if r.size == 0:
        return 0.0, 0.0
    return float(r.mean()), float(r.std())


def window_stats(a):
    """a: (T, 52) normalized amplitudes. Returns (mean[52], std[52], motion)."""
    a = hampel(a)
    mean = a.mean(axis=0)
    std = a.std(axis=0)
    # Overall wobble relative to the overall level. (Averaging each
    # subcarrier's own std/mean instead would let a few subcarriers sitting
    # in a deep fade - near-zero mean - dominate everything.)
    motion = 100.0 * float(std.mean() / max(float(mean.mean()), 1e-3))
    return mean, std, motion


def deviation(mean, base_mean):
    """Shape change vs the empty room, in percent."""
    return 100.0 * float(np.linalg.norm(mean - base_mean) / max(np.linalg.norm(base_mean), 1e-6))


def link_features(a, rssi, baseline):
    """
    One link, one window -> feature vector (LINK_FEATURES numbers) + summary.
    baseline: dict from build_baseline() for this link (needs "mean", "rssi").
    """
    mean, std, motion = window_stats(a)
    base_mean = np.asarray(baseline["mean"], dtype=np.float32)
    gbase = np.maximum(_group(base_mean), 1e-3)
    shape = 100.0 * _group(mean - base_mean) / gbase
    wobble = 100.0 * _group(std) / gbase
    dev = deviation(mean, base_mean)
    level, rssi_std = rssi_level(rssi)
    drssi = level - float(baseline["rssi"])
    vec = np.concatenate([shape, wobble, [dev, motion, drssi, rssi_std]]).astype(np.float32)
    return vec, {"motion": motion, "deviation": dev, "drssi": drssi, "rssi_std": rssi_std}


def combined_features(per_link, link_order):
    """
    per_link: {link_key: feature vector or None}. link_order: list of keys.
    Missing links become zeros plus a 0 "present" flag, so a model still
    runs if one transmitter is off (it will just be less accurate).
    """
    parts, present = [], []
    for key in link_order:
        vec = per_link.get(key)
        if vec is None:
            parts.append(np.zeros(LINK_FEATURES, np.float32))
            present.append(0.0)
        else:
            parts.append(vec)
            present.append(1.0)
    return np.concatenate(parts + [np.asarray(present, np.float32)])


# ----------------------------------------------------------------------------
#  Empty-room baseline (calibration)
# ----------------------------------------------------------------------------

def _robust(values, floor_abs, floor_rel):
    v = np.asarray(values, dtype=np.float64)
    mu = float(np.median(v))
    mad = float(np.median(np.abs(v - mu))) * 1.4826
    return mu, max(mad, floor_abs, floor_rel * mu)


def build_baseline(t, a, rssi, window_s=1.0):
    """
    Empty-room reference for one link from >= ~10 s of samples.
    Stores the average shape and how much deviation/motion an EMPTY room
    shows, so live values can be turned into "how unusual is this" scores.
    Returns None if there isn't enough data.
    """
    if len(t) < 5 * MIN_WINDOW_SAMPLES:
        return None
    mean = hampel(a).mean(axis=0)
    devs, motions, rssi_means, rssi_stds = [], [], [], []
    start = t[0]
    while start + window_s <= t[-1]:
        m = (t >= start) & (t < start + window_s)
        if m.sum() >= MIN_WINDOW_SAMPLES:
            wm, _ws, mo = window_stats(a[m])
            devs.append(deviation(wm, mean))
            motions.append(mo)
            level, wobble = rssi_level(rssi[m])
            rssi_means.append(level)
            rssi_stds.append(wobble)
        start += window_s / 2
    if len(devs) < 4:
        return None
    dev_mu, dev_sigma = _robust(devs, 0.4, 0.15)
    mot_mu, mot_sigma = _robust(motions, 0.3, 0.15)
    rssi_mu = float(np.median(rssi_means))
    rssi_jitter = max(0.25, float(np.median(np.abs(np.asarray(rssi_means) - rssi_mu))) * 1.4826)
    rstd_mu, rstd_sigma = _robust(rssi_stds, 0.15, 0.2)
    return {
        "mean": [round(float(x), 5) for x in mean],
        "std": [round(float(x), 5) for x in a.std(axis=0)],
        "rssi": round(rssi_mu, 2),
        "rssi_jitter": round(rssi_jitter, 3),            # dB, window-to-window
        "rssi_std_mu": round(rstd_mu, 3), "rssi_std_sigma": round(rstd_sigma, 3),
        "dev_mu": round(dev_mu, 4), "dev_sigma": round(dev_sigma, 4),
        "motion_mu": round(mot_mu, 4), "motion_sigma": round(mot_sigma, 4),
        "samples": int(len(t)), "windows": len(devs),
    }


def rssi_change(drssi_db, jitter_db, margin_db=1.0, scale_db=3.0):
    """
    0..1 from how far the link's power moved from the empty room (either
    way: a link sitting in a multipath fade can get STRONGER when blocked).
    """
    excess = abs(drssi_db) - margin_db - 2.0 * jitter_db
    return 1.0 - math.exp(-max(0.0, excess) / scale_db)


def disturbance(value, mu, sigma, z0=3.0, scale=10.0):
    """
    0..1 "how much is this link disturbed": 0 = like the empty room,
    approaching 1 = strongly blocked / moving. z0 = how many empty-room
    standard deviations count as noise.
    """
    z = (value - mu) / max(sigma, 1e-6)
    return 1.0 - math.exp(-max(0.0, z - z0) / scale)

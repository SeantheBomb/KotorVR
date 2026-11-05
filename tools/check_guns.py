"""Check the exported guns (kvr/models/*.kvrw): bolts fly along the model's -Z, so the visible barrel must lie along
-Z. Fits the barrel's centre line (centroids of slices across the front half) and prints the angle to -Z per gun.

usage: python check_guns.py "<game dir>" [max_deg=1.5]   -> exit 1 if any gun is off by more
"""
import glob
import math
import os
import struct
import sys

from weapon_export import barrel_axis, is_gun


def read_kvrw(path):
    d = open(path, "rb").read()
    assert d[:4] == b"KVRW"
    _ver, n = struct.unpack_from("<II", d, 4)
    o, pts = 12, []
    for _ in range(n):
        o += 32
        _flags, vc, ic = struct.unpack_from("<III", d, o)
        o += 12
        pts += [struct.unpack_from("<3f", d, o + i * 12) for i in range(vc)]
        o += vc * 12 + vc * 8 + ic * 2
        if ic % 2:
            o += 2
    return pts


def barrel_angle(pts):
    """The exporter's own barrel fit (weapon_export.barrel_axis), refined the same way, measured against -Z."""
    v = [0.0, 0.0, -1.0]
    for _ in range(6):
        v = barrel_axis(pts, v) or v
    return math.degrees(math.acos(min(1.0, abs(v[2]))))


def main():
    game = sys.argv[1]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
    bad = 0
    for p in sorted(glob.glob(os.path.join(game, "kvr", "models", "*.kvrw"))):
        r = os.path.basename(p)[:-5]
        if not is_gun(r) or not r.endswith("_001"):
            continue
        a = barrel_angle(read_kvrw(p))
        ok = a is not None and a <= limit
        bad += not ok
        print("%-18s barrel %5s deg off -Z  %s" % (r, "?" if a is None else "%.1f" % a, "ok" if ok else "OFF"))
    print("CHECK %s guns_barrel_along_bolt (%d off)" % ("PASS" if not bad else "FAIL", bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

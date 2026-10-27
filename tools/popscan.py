"""Find pop-in / pop-out candidates in a recording (simhost "record on" or kvr_host [Debug] Record=1).

Popping is a patch of the picture that changes for a frame or two and comes back - unlike camera motion, which
changes the picture smoothly. For each frame t the score is how different t is from both neighbours compared with
how different the neighbours are from each other, per 8x8 block (so a pop in one corner isn't averaged away), and
the worst block decides. Writes <video>_popscan.png: the top candidates as (t-1, t, t+1) strips, and prints them.

usage: python popscan.py <video.mp4> [top N=12]
"""
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

W, H = 240, 176


def frames(path):
    cmd = ["ffmpeg", "-loglevel", "error", "-i", path, "-vf", f"scale={W}:{H}", "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    n = len(raw) // (W * H)
    return np.frombuffer(raw[: n * W * H], np.uint8).reshape(n, H, W).astype(np.float32)


def blocks(d):
    return d.reshape(H // 8, 8, W // 8, 8).mean(axis=(1, 3))


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    f = frames(path)
    scores = []
    for t in range(1, len(f) - 1):
        a, b, c = f[t - 1], f[t], f[t + 1]
        transient = np.minimum(blocks(np.abs(b - a)), blocks(np.abs(b - c))) - blocks(np.abs(c - a))
        k = np.unravel_index(np.argmax(transient), transient.shape)
        scores.append((float(transient[k]), t, k))
    scores.sort(reverse=True)
    picks = []
    for s, t, k in scores:  # one per moment
        if all(abs(t - p[1]) > 4 for p in picks):
            picks.append((s, t, k))
        if len(picks) >= top:
            break
    # contact sheet
    rgb = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-vf", f"scale={W}:{H}", "-f", "rawvideo", "-pix_fmt",
                          "rgb24", "-"], capture_output=True, check=True).stdout
    n = len(rgb) // (W * H * 3)
    col = np.frombuffer(rgb[: n * W * H * 3], np.uint8).reshape(n, H, W, 3)
    sheet = Image.new("RGB", (W * 3, H * len(picks)), (0, 0, 0))
    for row, (s, t, (by, bx)) in enumerate(picks):
        for i, tt in enumerate((t - 1, t, t + 1)):
            im = Image.fromarray(col[tt])
            dr = ImageDraw.Draw(im)
            dr.rectangle([bx * 8 - 4, by * 8 - 4, bx * 8 + 12, by * 8 + 12], outline=(255, 0, 0))
            if i == 1:
                dr.text((4, 4), f"t={t / 15:.1f}s score {s:.0f}", fill=(255, 255, 0))
            sheet.paste(im, (i * W, row * H))
        print(f"t={t / 15:6.2f}s frame {t:4d} score {s:6.1f} block ({bx * 8},{by * 8})")
    out = path.rsplit(".", 1)[0] + "_popscan.png"
    sheet.save(out)
    print("sheet:", out)


if __name__ == "__main__":
    main()

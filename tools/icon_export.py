"""Export KotOR's GUI action icons (feats i_*, Force powers ip_*, skills isk_*, items iit_* / i*) from
TexturePacks/swpc_tex_gui.erf into <out>/icons/<resref>.png (top mip, RGBA) for the VR action ring.

usage: python icon_export.py <game_dir> <out_dir>
"""
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402
import weapon_export  # noqa: E402  (TPC parsing)

TYPE_TPC = 3007
PREFIXES = ("i_", "ip_", "isk_", "iit_", "ia_", "if_")


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def dxt_block_colors(c0, c1, dxt1):
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1 or not dxt1:
        return [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    return [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]


def decode_dxt(data, w, h, dxt1):
    img = bytearray(w * h * 4)
    bs = 8 if dxt1 else 16
    o = 0
    for by in range(0, max(1, h), 4):
        for bx in range(0, max(1, w), 4):
            alpha = None
            if not dxt1:
                a0, a1 = data[o], data[o + 1]
                bits = int.from_bytes(data[o + 2:o + 8], "little")
                if a0 > a1:
                    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                co = o + 8
            else:
                co = o
            c0, c1 = struct.unpack_from("<HH", data, co)
            idx = struct.unpack_from("<I", data, co + 4)[0]
            cols = dxt_block_colors(c0, c1, dxt1)
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x >= w or y >= h:
                    continue
                c = cols[(idx >> (2 * i)) & 3]
                p = (y * w + x) * 4
                img[p:p + 3] = bytes(c[:3])
                img[p + 3] = alpha[i] if alpha is not None else c[3]
            o += bs
    return bytes(img)


def tpc_to_image(data):
    kv = weapon_export.tpc_to_kvrt(data)
    fmt, w, h, n = struct.unpack_from("<IIII", kv, 4)
    size = struct.unpack_from("<I", kv, 20)[0]
    level = kv[24:24 + size]
    if fmt == 0:
        return Image.frombytes("RGBA", (w, h), level)
    return Image.frombytes("RGBA", (w, h), decode_dxt(level, w, h, fmt == 1))


def main(game, out):
    ents, data = kres.read_erf(os.path.join(game, "TexturePacks", "swpc_tex_gui.erf"))
    os.makedirs(os.path.join(out, "icons"), exist_ok=True)
    n = 0
    for (name, t), (off, size) in ents.items():
        if t != TYPE_TPC or not name.startswith(PREFIXES):
            continue
        try:
            im = tpc_to_image(data[off:off + size])
            # TPC rows are stored bottom-up for GUI textures in KotOR: flip to read normally
            im = im.transpose(Image.FLIP_TOP_BOTTOM)
            im.save(os.path.join(out, "icons", name + ".png"))
            n += 1
        except Exception as e:  # one odd texture must not stop the export
            print(f"{name}: {e}")
    print(f"{n} icons -> {os.path.join(out, 'icons')}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

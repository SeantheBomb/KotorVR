r"""Export KotOR's own GUI font for the VR host's text panels: the small-caps menu face (dialogfont32x32, the sharp
version of dialogfont16x16) as white glyphs with alpha, and its glyph boxes from the texture's TXI.

    python font_export.py <game_dir> <out_dir>      # writes <out>\fonts\kotor.png and kotor.txt

kotor.txt: one line per character code that has a glyph: "<code> <x0> <y0> <x1> <y1>" in pixels of kotor.png
(top-left origin). Derived from the user's own game files; stays local.
"""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import icon_export  # noqa: E402  (TPC decoding)
import kres  # noqa: E402

FONT = "dialogfont32x32"


def coords(txi, key):
    lines = txi.replace("\r", "").split("\n")
    for i, l in enumerate(lines):
        parts = l.split()
        if parts and parts[0] == key:
            n = int(parts[1])
            return [tuple(float(v) for v in lines[i + 1 + k].split()[:2]) for k in range(n)]
    return []


def main(game, out):
    ents, data = kres.read_erf(os.path.join(game, "TexturePacks", "swpc_tex_gui.erf"))
    off, size = ents[(FONT, 3007)]
    raw = data[off:off + size]
    im = icon_export.tpc_to_image(raw).transpose(Image.FLIP_TOP_BOTTOM).convert("RGBA")
    w, h = im.size
    txi = raw[raw.find(b"mipmap"):].decode("latin1", "replace")
    ul, lr = coords(txi, "upperleftcoords"), coords(txi, "lowerrightcoords")
    # white glyphs, coverage in alpha (the texture is tinted blue; the host tints it per line)
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            px[x, y] = (255, 255, 255, a)
    os.makedirs(os.path.join(out, "fonts"), exist_ok=True)
    im.save(os.path.join(out, "fonts", "kotor.png"))
    lines = []
    for code, (a, b) in enumerate(zip(ul, lr)):
        x0, y0 = a[0] * w, (1 - a[1]) * h
        x1, y1 = b[0] * w, (1 - b[1]) * h
        if x1 - x0 < 1 or y1 - y0 < 1:
            continue
        # an empty box (the placeholder rectangle KotOR draws for unknown codes) is left out
        box = im.crop((int(x0), int(y0), int(x1), int(y1)))
        if code != 32 and not box.getbbox():
            continue
        lines.append("%d %.1f %.1f %.1f %.1f" % (code, x0, y0, x1, y1))
    open(os.path.join(out, "fonts", "kotor.txt"), "w").write("\n".join(lines) + "\n")
    print("%d glyphs -> %s" % (len(lines), os.path.join(out, "fonts")))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

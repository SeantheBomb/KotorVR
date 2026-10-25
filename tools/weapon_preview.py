"""Side-view silhouettes of exported .kvrw weapons (grip frame: -Z forward -> right, +Y up), origin in red.
    python weapon_preview.py <models_dir> <out.png> name1 name2 ...
"""
import struct, sys
from PIL import Image, ImageDraw


def load(p):
    d = open(p, 'rb').read(); n = struct.unpack_from('<I', d, 8)[0]; o = 12; meshes = []
    for _ in range(n):
        tex = d[o:o + 32].split(b'\0')[0].decode(); fl, vc, ic = struct.unpack_from('<III', d, o + 32); o += 44
        pos = struct.unpack_from('<%df' % (vc * 3), d, o); o += vc * 12; o += vc * 8
        idx = struct.unpack_from('<%dH' % ic, d, o); o += ic * 2 + (2 if ic % 2 else 0)
        meshes.append((tex, fl, [pos[i * 3:i * 3 + 3] for i in range(vc)], idx))
    return meshes


def main(mdir, out, names):
    W, H, S = 260, 300, 120
    img = Image.new('RGB', (W * len(names), H), (20, 20, 24)); dr = ImageDraw.Draw(img)
    for ci, nm in enumerate(names):
        ox, oy = ci * W + W * 0.25, H * 0.55
        for tex, fl, pos, idx in load(f'{mdir}/{nm}.kvrw'):
            col = (90, 170, 255) if fl else (200, 200, 205)
            for t in range(0, len(idx) - 2, 3):
                pts = [(ox + -pos[i][2] * S, oy - pos[i][1] * S) for i in idx[t:t + 3]]
                dr.polygon(pts, fill=col)
        dr.ellipse((ox - 3, oy - 3, ox + 3, oy + 3), fill=(255, 60, 60))
        dr.text((ci * W + 4, 4), nm, fill=(255, 255, 255))
    img.save(out)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3:])

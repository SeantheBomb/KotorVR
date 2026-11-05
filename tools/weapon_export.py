"""Export KotOR weapon models + textures into compact files the game layer draws in the player's hands.

    python weapon_export.py <game_dir> <out_dir>

For every w_* model: bake the node tree into one frame whose origin is the model root (KotOR attaches it to
the hand there), whose -Z points at the weapon's tip and whose +Y points "up" (toward the mesh centroid's side,
e.g. a pistol's body above its handle). Writes <out>/models/<resref>.kvrw and <out>/textures/<name>.kvrt.
These are derived from the user's own game files and stay local.

kvrw: 'KVRW' u32 version=1, u32 meshCount, then per mesh:
      char texture[32], u32 flags (1 = additive saber blade), u32 vcount, u32 icount,
      f32 pos[v][3], f32 uv[v][2], u16 idx[icount]  (+2 pad bytes if icount is odd)
kvrt: 'KVRT' u32 format (0 RGBA8, 1 DXT1, 5 DXT5), u32 width, u32 height, u32 mips, then per mip: u32 size, data
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402
import mdl as mdlmod  # noqa: E402

TYPE_MDL, TYPE_MDX, TYPE_TPC, TYPE_TGA = 2002, 3008, 3007, 3


def qmat(q):  # (w, x, y, z) -> 3x3 rows
    w, x, y, z = q
    n = math.sqrt(w * w + x * x + y * y + z * z) or 1
    w, x, y, z = w / n, x / n, y / n, z / n
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def mmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def mvec(a, v):
    return [sum(a[r][k] * v[k] for k in range(3)) for r in range(3)]


def norm(v):
    l = math.sqrt(sum(x * x for x in v))
    return [x / l for x in v] if l > 1e-9 else None


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def bake(model):
    """[(texture, flags, positions(root space), uvs, tri indices)]"""
    out = []

    def rec(node, R, t):
        Rl = qmat(node.orientation)
        Rn = mmul(R, Rl)
        tn = [t[i] + mvec(R, node.position)[i] for i in range(3)]
        me = node.mesh
        if me and me.render and me.faces and me.texture and me.texture.lower() != "null":
            pos = [[tn[i] + mvec(Rn, p)[i] for i in range(3)] for p in me.positions]
            saber = bool(node.type & mdlmod.NODE_SABER) or me.texture.lower().startswith("w_lsabre")
            out.append((me.texture.lower(), 1 if saber else 0, pos, me.uvs, me.faces))
        for c in node.children:
            rec(c, Rn, tn)

    rec(model.root, [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
    return out


GUN_WORDS = ("blstr", "pstl", "rfl", "crbn", "bowc", "rptn", "dsrpt")


def is_gun(resref):
    r = resref.lower()
    return any(w in r for w in GUN_WORDS) and "gren" not in r


def barrel_axis(pts, fwd):
    """A gun's barrel direction: the line through the centroids of slices across its front half (least squares).
    The farthest point from the grip (the muzzle tip) is not on the barrel's axis line from the grip - the barrel
    sits above the hand - so 'grip -> tip' ran 8-17 deg off the visible barrel (2026-11-05)."""
    proj = [sum(p[i] * fwd[i] for i in range(3)) for p in pts]
    lo, hi = min(proj), max(proj)
    cs = []
    for k in range(8):
        a = lo + (hi - lo) * (0.45 + 0.07 * k)
        b = a + (hi - lo) * 0.07
        sl = [p for p, s in zip(pts, proj) if a <= s < b]
        if len(sl) >= 3:
            cs.append([sum(p[i] for p in sl) / len(sl) for i in range(3)])
    if len(cs) < 3:
        return None
    m = [sum(c[i] for c in cs) / len(cs) for i in range(3)]
    cov = [[sum((c[i] - m[i]) * (c[j] - m[j]) for c in cs) for j in range(3)] for i in range(3)]
    v = fwd[:]
    for _ in range(50):  # principal direction (power iteration)
        v = norm([sum(cov[i][j] * v[j] for j in range(3)) for i in range(3)]) or fwd
    if sum(v[i] * fwd[i] for i in range(3)) < 0:
        v = [-x for x in v]
    return v


def grip_frame(meshes, gun=False):
    """Rotation rows mapping root space -> grip frame (tip along -Z, 'up' along +Y). Guns: -Z along the barrel."""
    pts = [p for _, _, pos, _, _ in meshes for p in pos]
    blade = [p for _, f, pos, _, _ in meshes if f for p in pos]
    tip_src = blade or pts
    tip = max(tip_src, key=lambda p: p[0] ** 2 + p[1] ** 2 + p[2] ** 2)
    fwd = norm(tip) or [0, 0, 1]
    if gun:
        for _ in range(6):  # re-slice along each new estimate until it settles
            fwd = barrel_axis(pts, fwd) or fwd
    c = [sum(p[i] for p in pts) / len(pts) for i in range(3)]
    side = [c[i] - fwd[i] * sum(c[k] * fwd[k] for k in range(3)) for i in range(3)]
    up = norm(side)
    if up is None or math.sqrt(sum(x * x for x in side)) < 0.01:
        # no clear side: pick the root axis most perpendicular to the tip direction
        axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        a = min(axes, key=lambda ax: abs(sum(ax[i] * fwd[i] for i in range(3))))
        up = norm([a[i] - fwd[i] * sum(a[k] * fwd[k] for k in range(3)) for i in range(3)])
    right = cross(up, [-x for x in fwd])  # x = y cross z, z = -fwd
    # rows: grip x, y, z expressed in root space
    return [right, up, [-x for x in fwd]]


def blade_edge_up(meshes, rows):
    """Blades: the flat must face the palm (thin along X, edge along Y) so a natural thumb-up grip holds the edge
    forward. Models whose blade (beyond 15 cm from the grip) is wider in X than in Y get a 90 deg turn about the
    blade axis."""
    xs, ys = [], []
    for _t, flags, pos, _uv, _f in meshes:
        if flags & 1:  # saber blades are round
            continue
        for p in pos:
            q = mvec(rows, p)
            if q[2] < -0.15:
                xs.append(q[0])
                ys.append(q[1])
    if xs and (max(xs) - min(xs)) > 1.5 * (max(ys) - min(ys)) and (max(xs) - min(xs)) > 0.03:
        rz = [[0, 1, 0], [-1, 0, 0], [0, 0, 1]]  # -90 deg: x -> -y (+90 left vibroblade edges facing back, 2026-10-26)
        return mmul(rz, rows)
    return rows


def write_model(path, meshes, rows):
    with open(path, "wb") as f:
        f.write(b"KVRW" + struct.pack("<II", 1, len(meshes)))
        for tex, flags, pos, uvs, faces in meshes:
            idx = [i for tri in faces for i in tri]
            f.write(tex.encode("latin1")[:31].ljust(32, b"\0"))
            f.write(struct.pack("<III", flags, len(pos), len(idx)))
            for p in pos:
                f.write(struct.pack("<3f", *mvec(rows, p)))
            for uv in uvs:
                f.write(struct.pack("<2f", uv[0], uv[1]))
            f.write(struct.pack("<%dH" % len(idx), *idx))
            if len(idx) % 2:
                f.write(b"\0\0")


def tpc_to_kvrt(data):
    ds, _alpha, w, h, enc, mips = struct.unpack_from("<IfHHBB", data, 0)
    p = 128
    levels = []
    if ds:  # compressed
        fmt = 1 if enc == 2 else 5
        bs = 8 if fmt == 1 else 16
        for i in range(mips):
            mw, mh = max(1, w >> i), max(1, h >> i)
            size = max(1, (mw + 3) // 4) * max(1, (mh + 3) // 4) * bs
            if p + size > len(data):
                break
            levels.append(data[p:p + size])
            p += size
    else:
        fmt = 0
        bpp = {1: 1, 2: 3, 4: 4}.get(enc, 4)
        for i in range(max(1, mips)):
            mw, mh = max(1, w >> i), max(1, h >> i)
            size = mw * mh * bpp
            if p + size > len(data):
                break
            raw = data[p:p + size]
            if bpp != 4:  # expand to RGBA8
                px = bytearray()
                for k in range(mw * mh):
                    s = raw[k * bpp:(k + 1) * bpp]
                    px += (s * 3 + b"\xff") if bpp == 1 else (s + b"\xff")
                raw = bytes(px)
            levels.append(raw)
            p += size
    out = b"KVRT" + struct.pack("<IIII", fmt, w, h, len(levels))
    for lv in levels:
        out += struct.pack("<I", len(lv)) + lv
    return out


def main(game, out):
    os.makedirs(os.path.join(out, "models"), exist_ok=True)
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    bifs, keys = kres.read_key(game)
    packs = []
    for name in ("swpc_tex_tpa.erf", "swpc_tex_tpb.erf", "swpc_tex_tpc.erf"):
        p = os.path.join(game, "TexturePacks", name)
        if os.path.exists(p):
            packs.append(kres.read_erf(p))
    textures = set()
    models = sorted(r for (r, t) in keys if t == TYPE_MDL and r.startswith("w_"))
    for r in models:
        try:
            m = mdlmod.Model(kres.extract(game, r, "mdl"), kres.extract(game, r, "mdx"))
            meshes = bake(m)
            if not meshes:
                print(f"{r}: no renderable meshes")
                continue
            rows = blade_edge_up(meshes, grip_frame(meshes, is_gun(r)))
            write_model(os.path.join(out, "models", r + ".kvrw"), meshes, rows)
            textures.update(t for t, *_ in meshes)
            print(f"{r}: {len(meshes)} meshes, {sum(len(x[2]) for x in meshes)} verts")
        except Exception as e:  # keep going: one odd model must not stop the export
            print(f"{r}: FAILED {e}")
    for t in sorted(textures):
        data = None
        for ents, d in packs:
            k = (t, TYPE_TPC)
            if k in ents:
                off, size = ents[k]
                data = d[off:off + size]
                break
        if data is None:
            print(f"texture {t}: not found in texture packs")
            continue
        open(os.path.join(out, "textures", t + ".kvrt"), "wb").write(tpc_to_kvrt(data))
    # base item row -> model prefix, so the game layer can name w_<class>_<variation> without parsing 2DAs
    cols, labels, rows = kres.read_2da(kres.extract(game, "baseitems", "2da"))
    ic = cols.index("itemclass")
    with open(os.path.join(out, "models", "baseitems.txt"), "w") as f:
        for lab, row in zip(labels, rows):
            if row[ic] and row[ic] != "****":
                f.write(f"{lab} {row[ic].lower()}\n")
    print(f"{len(models)} models, {len(textures)} textures -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

"""Export the player-visible hands from KotOR's own body models (so they wear the outfit: gloves, sleeves, skin).

    python hand_export.py <game_dir> <out_dir>      (out_dir = <game>\\kvr)

Hands are part of the skinned arm meshes (armL / armR). For every body model named in appearance.2da we take the
triangles whose vertices are dominated by hand / finger / thumb bones, move them into the hand bone's frame (bind
pose), and split them per finger segment with the joint pivots so the game can curl the fingers.

Writes <out>/hands/<model>_<r|l>.kvrh, <out>/hands/appearance.txt (row -> model / texture per body letter a..j)
and the body textures to <out>/textures/<name>.kvrt (same format as the weapons).

kvrh v2 (skinned): 'KVRH' u32 version=2, char texture[32], u32 segCount, then per segment:
      char name[16], i32 parent (segment index or -1), f32 pivot[3] (hand frame);
      then u32 vcount, u32 icount, f32 pos[v][3], f32 uv[v][2], u8 bone[v][4] (segment indices), f32 weight[v][4],
      u16 idx[icount] (+2 pad bytes if icount is odd)
Hand frame = the hand bone's bind-pose frame (KotOR: fingers along -Z, origin at the wrist). Segment 0 is the palm;
weights on bones outside the hand (forearm) go to the palm, so the wrist blends instead of tearing.
"""
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402
import mdl as mdlmod  # noqa: E402
from weapon_export import qmat, mmul, mvec, tpc_to_kvrt, TYPE_MDL, TYPE_MDX, TYPE_TPC  # noqa: E402

LETTERS = "abcdefghij"


def node_worlds(model):
    """name(lower) -> (R rows, t) model-space bind transforms"""
    out = {}

    def rec(node, R, t):
        Rn = mmul(R, qmat(node.orientation))
        tn = [t[i] + mvec(R, node.position)[i] for i in range(3)]
        out[node.name.lower()] = (Rn, tn)
        for c in node.children:
            rec(c, Rn, tn)

    rec(model.root, [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
    return out


def hand_bone(name, side):
    n = name.lower()
    return n.startswith(side) and ("hand" in n or "fngr" in n or "thumb" in n)


def export_hand(model, side):
    """side 'r' / 'l' -> (texture, segments) or None"""
    worlds = node_worlds(model)
    by_idx = model.by_name_index()
    hand_name = side + "hand_g"
    if hand_name not in worlds:
        return None
    Rh, th = worlds[hand_name]
    RhT = [[Rh[c][r] for c in range(3)] for r in range(3)]

    def to_hand(p):
        d = [p[i] - th[i] for i in range(3)]
        return mvec(RhT, d)

    for n, _ in model.walk():
        if not (n.type & mdlmod.NODE_SKIN) or not n.mesh or not n.mesh.faces:
            continue
        me = n.mesh
        Rs, ts = worlds[n.name.lower()]
        dom = []
        for w, b in zip(me.weights, me.bones):
            k = max(range(4), key=lambda j: w[j])
            bn = by_idx.get(b[k])
            dom.append(bn.name.lower() if bn else "")
        if sum(1 for d in dom if hand_bone(d, side)) < 20:
            continue  # not this side's arm
        segs = {}
        for f in me.faces:
            names = [dom[v] for v in f]
            if not all(hand_bone(x, side) for x in names):
                continue
            seg = Counter(names).most_common(1)[0][0]
            if not seg.endswith("_g"):
                seg = hand_name  # 'rhand' attach dummy -> palm
            segs.setdefault(seg, []).append(f)
        if not segs:
            continue
        order = sorted(segs, key=lambda s: (s != hand_name, s))  # palm first
        seg_index = {name: i for i, name in enumerate(order)}
        segments = []
        for name in order:
            base = name.replace("fngrt", "fngrb").replace("thumbt", "thumbb")
            parent = seg_index.get(base, -1) if base != name else -1
            pivot = to_hand(worlds[name][1]) if name in worlds else [0, 0, 0]
            segments.append((name, parent, pivot))
        # one skinned mesh over all the hand's faces; each vertex keeps its 4 weights, bones mapped to segments
        remap, pos, uv, bones, weights, idx = {}, [], [], [], [], []
        for name in order:
            for f in segs[name]:
                for v in f:
                    if v not in remap:
                        remap[v] = len(pos)
                        wp = [ts[i] + mvec(Rs, me.positions[v])[i] for i in range(3)]
                        pos.append(to_hand(wp))
                        uv.append(me.uvs[v])
                        acc = {}
                        for w, b in zip(me.weights[v], me.bones[v]):
                            if w <= 0 or b < 0:
                                continue
                            bn = by_idx.get(b)
                            nm = bn.name.lower() if bn else ""
                            si = seg_index.get(nm, 0) if hand_bone(nm, side) else 0
                            acc[si] = acc.get(si, 0.0) + w
                        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4] or [(0, 1.0)]
                        total = sum(w for _, w in top) or 1.0
                        top += [(0, 0.0)] * (4 - len(top))
                        bones.append([b for b, _ in top])
                        weights.append([w / total for _, w in top])
                    idx.append(remap[v])
        return me.texture.lower(), segments, pos, uv, bones, weights, idx
    return None


def write_kvrh(path, texture, segments, pos, uv, bones, weights, idx):
    with open(path, "wb") as f:
        f.write(b"KVRH" + struct.pack("<I", 2) + texture.encode()[:31].ljust(32, b"\0") + struct.pack("<I", len(segments)))
        for name, parent, pivot in segments:
            f.write(name.encode()[:15].ljust(16, b"\0") + struct.pack("<i3f", parent, *pivot))
        f.write(struct.pack("<II", len(pos), len(idx)))
        for p in pos:
            f.write(struct.pack("<3f", *p))
        for t in uv:
            f.write(struct.pack("<2f", *t))
        for b in bones:
            f.write(struct.pack("<4B", *b))
        for w in weights:
            f.write(struct.pack("<4f", *w))
        f.write(struct.pack("<%dH" % len(idx), *idx))
        if len(idx) % 2:
            f.write(b"\0\0")


def main(game, out):
    os.makedirs(os.path.join(out, "hands"), exist_ok=True)
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    _, keys = kres.read_key(game)
    cols, labels, rows = kres.read_2da(kres.extract(game, "appearance", "2da"))
    table, models, texbases = [], set(), set()
    for label, r in zip(labels, rows):
        d = dict(zip(cols, r))
        if d.get("modeltype", "").upper() != "B":
            continue
        ms = [(d.get("model" + L, "") or "-").lower().replace("****", "-") for L in LETTERS]
        ts = [(d.get("tex" + L, "") or "-").lower().replace("****", "-") for L in LETTERS]
        table.append(f"{label} " + " ".join(ms) + " " + " ".join(ts))
        models.update(m for m in ms if m != "-")
        texbases.update(t for t in ts if t != "-")
    with open(os.path.join(out, "hands", "appearance.txt"), "w") as f:
        f.write("# row model_a..model_j tex_a..tex_j (appearance.2da, body models only)\n" + "\n".join(table) + "\n")
    done = 0
    for m in sorted(models):
        if (m, TYPE_MDL) not in keys or (m, TYPE_MDX) not in keys:
            continue
        try:
            model = mdlmod.Model(kres.extract(game, m, "mdl"), kres.extract(game, m, "mdx"))
        except Exception as e:  # noqa: BLE001
            print(f"{m}: {e}")
            continue
        for side in "rl":
            res = export_hand(model, side)
            if res:
                write_kvrh(os.path.join(out, "hands", f"{m}_{side}.kvrh"), *res)
                done += 1
    # body textures (texture packs, like the weapons): every <tex base> and <tex base><NN> the game has
    packs = []
    for name in ("swpc_tex_tpa.erf", "swpc_tex_tpb.erf", "swpc_tex_tpc.erf"):
        pth = os.path.join(game, "TexturePacks", name)
        if os.path.exists(pth):
            packs.append(kres.read_erf(pth))
    wanted = {}
    for ents, d in packs:
        for (r, t), (off, size) in ents.items():
            if t != TYPE_TPC or r in wanted:
                continue
            if any(r == b or (r.startswith(b) and r[len(b):].isdigit()) for b in texbases):
                wanted[r] = d[off:off + size]
    for t, data in sorted(wanted.items()):
        dst = os.path.join(out, "textures", t + ".kvrt")
        if not os.path.exists(dst):
            open(dst, "wb").write(tpc_to_kvrt(data))
    tex_names = wanted
    print(f"{done} hands from {len(models)} body models, {len(tex_names)} body textures -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

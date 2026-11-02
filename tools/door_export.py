"""Door boxes for the hand's ray pick (game/pick.cpp): every door placed in every module, with its facing and the
extent of its model, so pointing anywhere on a door (a 6 m Endar Spire door, its lock panel) picks it.

The game only exposes a door's position at runtime; its model and bearing come from the module's GIT and the door
template (UTD GenericType -> genericdoors.2da modelname). Runtime matches doors by position.

usage: python door_export.py "<game dir>" "<game dir>\\kvr"     -> <out>\\models\\doors.txt
line:  x y z bearing xmin xmax ymin ymax zmin zmax model   (model-local extents, metres; bearing in radians)
"""
import glob
import os
import struct
import sys

import kres
import mdl

SKIP_NODES = {"trans", "blackpoly"}  # door-way portal / blocker polys, not the door itself


def read_rim(path):
    d = open(path, "rb").read()
    assert d[:8] == b"RIM V1.0", d[:8]
    n, off = struct.unpack_from("<II", d, 12)
    out = {}
    for i in range(n):
        o = off + i * 32
        rr = d[o:o + 16].rstrip(b"\0").decode("latin1").lower()
        rtype, _id, roff, rsize = struct.unpack_from("<IIII", d, o + 16)
        out[(rr, rtype)] = d[roff:roff + rsize]
    return out


def read_mod(path):
    idx, d = kres.read_erf(path)
    return {k: d[o:o + s] for k, (o, s) in idx.items()}


def gff(data):
    (_ft, _fv, so, _sc, fo, _fc, lo, lc, fido, _fids, fio, _fis, lio, _lis) = struct.unpack_from("<4s4s12I", data)
    labels = [data[lo + i * 16:lo + i * 16 + 16].rstrip(b"\0").decode("latin1") for i in range(lc)]

    def field(i):
        t, li, v = struct.unpack_from("<III", data, fo + i * 12)
        lab = labels[li]
        if t in (0, 1, 2, 3, 4, 5):
            return lab, v
        if t == 8:
            return lab, struct.unpack("<f", struct.pack("<I", v))[0]
        if t == 11:
            ln = data[fido + v]
            return lab, data[fido + v + 1:fido + v + 1 + ln].decode("latin1")
        if t == 14:
            return lab, struct_(v)
        if t == 15:
            cnt = struct.unpack_from("<I", data, lio + v)[0]
            return lab, [struct_(x) for x in struct.unpack_from("<%dI" % cnt, data, lio + v + 4)]
        return lab, None

    def struct_(si):
        _t, di, cnt = struct.unpack_from("<III", data, so + si * 12)
        idx = [di] if cnt == 1 else struct.unpack_from("<%dI" % cnt, data, fio + di)
        return dict(field(i) for i in idx)

    return struct_(0)


def model_extent(game, name, cache):
    if name in cache:
        return cache[name]
    ext = None
    try:
        m = mdl.Model(kres.extract(game, name, "mdl"), kres.extract(game, name, "mdx"))
        xs, ys, zs = [], [], []

        def walk(n, off):
            o = (off[0] + n.position[0], off[1] + n.position[1], off[2] + n.position[2])
            if n.mesh and n.mesh.render and n.name.lower() not in SKIP_NODES:
                for p in n.mesh.positions:
                    xs.append(p[0] + o[0])
                    ys.append(p[1] + o[1])
                    zs.append(p[2] + o[2])
            for c in n.children:
                walk(c, o)

        walk(m.root, (0, 0, 0))
        if xs:
            ext = (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs))
    except Exception:  # noqa: BLE001 - a model we can't read just falls back to the runtime default
        pass
    cache[name] = ext
    return ext


def main(game=None, out=None):
    game, out = game or sys.argv[1], out or sys.argv[2]
    _cols, _labels, rows = kres.read_2da(kres.extract(game, "genericdoors", "2da"))
    mi = _cols.index("modelname")
    # chitin UTDs (templates not packed into the module)
    lines, cache, mods = [], {}, {}
    for p in glob.glob(os.path.join(game, "modules", "*")):
        base = os.path.basename(p).lower()
        if base.endswith("_s.rim"):
            continue
        name = base.rsplit(".", 1)[0]
        try:
            res = read_mod(p) if base.endswith(".mod") else read_rim(p)
            s = os.path.join(game, "modules", name + "_s.rim")
            if base.endswith(".rim") and os.path.exists(s):
                res.update(read_rim(s))
        except Exception:  # noqa: BLE001
            continue
        mods[name] = res
    for name, res in sorted(mods.items()):
        gits = [v for k, v in res.items() if k[1] == 2023]
        if not gits:
            continue
        for d in gff(gits[0]).get("Door List", []) or []:
            tr = (d.get("TemplateResRef") or "").lower()
            utd = res.get((tr, 2042))
            if utd is None:
                try:
                    utd = kres.extract(game, tr, "utd") if tr else None
                except Exception:  # noqa: BLE001
                    utd = None
            gt = gff(utd).get("GenericType") if utd else None
            if gt is None or gt >= len(rows):
                continue
            model = rows[gt][mi].lower()
            ext = model_extent(game, model, cache)
            if not ext:
                continue
            lines.append("%.3f %.3f %.3f %.4f %.2f %.2f %.2f %.2f %.2f %.2f %s" % (
                d["X"], d["Y"], d["Z"], d.get("Bearing", 0.0), *ext, model))
    os.makedirs(os.path.join(out, "models"), exist_ok=True)
    path = os.path.join(out, "models", "doors.txt")
    with open(path, "w") as f:
        f.write("# x y z bearing xmin xmax ymin ymax zmin zmax model  (tools/door_export.py)\n")
        f.write("\n".join(lines) + "\n")
    print("%d doors in %d modules -> %s" % (len(lines), len(mods), path))


if __name__ == "__main__":
    main()

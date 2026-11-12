r"""The choreography table (combat design S4): where each humanoid combat animation puts the blades, and when it lands.

    python choreo_export.py <game_dir> <out_file>      # e.g. <game>\kvr\choreo.txt

KotOR picks a paired animation for both fighters at the roll (combatanimations.2da), and the damage lands on the
attack animation's Hit event. This samples every humanoid attack / parry / damage animation from the game's own
supermodels (the player body chain pmbam -> s_female02 -> s_female01 -> s_male02 -> s_male01) by forward kinematics
over the animated skeleton, and writes the right / left weapon root ("rhand" / "lhand" hooks) as a blade segment in
model space (KotOR metres, z up, model facing +y): a saber's blade runs along the hook's +z from 0.18 to 1.16 m
(w_lghtsbr_*, w_dblsbr_* second blade along -z).

Output, one block per animation:
    anim <name> <length s> <hit s or -1> <transtime s> <samples>
    s <t> <rb xyz> <rt xyz> <rt2 xyz> <lb xyz> <lt xyz>     (right base, right tip, right second tip (-z), left base, left tip)
Samples run every 1/30 s from 0 to the animation's length. Derived from the user's own game files; stays local.
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402
import mdlanim as M  # noqa: E402

CHAIN = ["s_male02", "s_male01", "s_female02", "s_female01"]  # skeleton from s_male02; animations from any
BLADE0, BLADE1 = 0.18, 1.16
PREFIXES = ("c", "f", "g", "m")  # combat attack / parry / damage rows (c2a1, f2a1, g2a1, m2a1, c2p1, c2d1, ...)


def qmul(a, b):  # (x, y, z, w)
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))


def slerp(a, b, f):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = tuple(-x for x in b), -d
    if d > 0.9995:
        r = tuple(x + (y - x) * f for x, y in zip(a, b))
    else:
        th = math.acos(d)
        s0, s1 = math.sin((1 - f) * th) / math.sin(th), math.sin(f * th) / math.sin(th)
        r = tuple(x * s0 + y * s1 for x, y in zip(a, b))
    n = math.sqrt(sum(x * x for x in r)) or 1
    return tuple(x / n for x in r)


def sample(c, t):
    times, vals = c.times, c.values
    if len(times) == 1 or t <= times[0]:
        return tuple(float(v) for v in vals[0])
    if t >= times[-1]:
        return tuple(float(v) for v in vals[-1])
    i = int(np.searchsorted(times, t)) - 1
    f = (t - times[i]) / max(times[i + 1] - times[i], 1e-6)
    a, b = tuple(float(v) for v in vals[i]), tuple(float(v) for v in vals[i + 1])
    if len(a) == 4:
        return slerp(a, b, f)
    return tuple(x + (y - x) * f for x, y in zip(a, b))


def rest(node):
    p, o = node.ctrl(M.CTRL_POSITION), node.ctrl(M.CTRL_ORIENTATION)
    pos = tuple(float(v) for v in p.values[0]) if p is not None and len(p.values) and p.values.shape[1] == 3 else tuple(node.position)
    q = tuple(float(v) for v in o.values[0]) if o is not None and len(o.values) and o.values.shape[1] == 4 else tuple(node.orientation)
    return pos, q


def pose(skel, anim_by_name, t):
    """World (model-space) position and rotation of every skeleton node at time t."""
    out = {}

    def walk(n, ppos, pq):
        pos, q = rest(n)
        a = anim_by_name.get(n.name.lower())
        if a is not None:
            pc, oc = a.ctrl(M.CTRL_POSITION), a.ctrl(M.CTRL_ORIENTATION)
            if pc is not None and len(pc.values) and pc.values.shape[1] == 3:
                d = sample(pc, t)
                pos = (pos[0] + d[0], pos[1] + d[1], pos[2] + d[2])
            if oc is not None and len(oc.values) and oc.values.shape[1] == 4:
                q = sample(oc, t)
        r = qrot(pq, pos)
        wpos = (ppos[0] + r[0], ppos[1] + r[1], ppos[2] + r[2])
        wq = qmul(pq, q)
        out[n.name.lower()] = (wpos, wq)
        for ch in n.children:
            walk(ch, wpos, wq)

    walk(skel.root, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))
    return out


def blade(p, name, z0, z1):
    pos, q = p[name]
    a, b = qrot(q, (0, 0, z0)), qrot(q, (0, 0, z1))
    return (pos[0] + a[0], pos[1] + a[1], pos[2] + a[2]), (pos[0] + b[0], pos[1] + b[1], pos[2] + b[2])


def main(game, out_path):
    models = {n: M.Model(kres.extract(game, n, "mdl"), kres.extract(game, n, "mdx")) for n in CHAIN}
    skel = models["s_male02"]
    anims = {}
    for n in CHAIN:  # first model in the chain that has an animation wins (the engine's supermodel lookup order)
        for a in models[n].animations:
            nm = a.name.lower()
            if nm in anims or not nm.startswith(PREFIXES) or len(nm) != 4 or not nm[1].isdigit():
                continue
            anims[nm] = a
    lines = ["# KotOR VR choreography table (tools/choreo_export.py); model space, metres, z up, facing +y"]
    for nm in sorted(anims):
        a = anims[nm]
        by = {an.name.lower(): an for an in a.nodes}
        hits = [t for t, e in a.events if e.lower() == "hit"]
        hit = hits[0] if hits else -1.0
        steps = max(2, int(a.length * 30) + 1)
        lines.append("anim %s %.4f %.4f %.4f %d" % (nm, a.length, hit, a.transtime, steps))
        for i in range(steps):
            t = min(a.length, i / 30.0)
            p = pose(skel, by, t)
            rb, rt = blade(p, "rhand", BLADE0, BLADE1)
            _, rt2 = blade(p, "rhand", -BLADE0, -BLADE1)
            lb, lt = blade(p, "lhand", BLADE0, BLADE1)
            v = rb + rt + rt2 + lb + lt
            lines.append("s %.4f " % t + " ".join("%.4f" % x for x in v))
    open(out_path, "w").write("\n".join(lines) + "\n")
    print("%d animations -> %s" % (len(anims), out_path))
    for nm in ("c2a1", "c2a4", "c2p1"):
        if nm in anims:
            a = anims[nm]
            print(" ", nm, "length %.3f" % a.length, "events", [(round(t, 3), e) for t, e in a.events])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

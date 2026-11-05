"""Walking routes for the test harness's autopilot (game/simcmd.cpp "goto"): A* over a module's walkmesh.

The module's rooms (<module>.lyt) each have a walkmesh (<room>.wok, BWM V1.0); their walkable triangles (surfacemat.2da
Walk=1) joined by shared edges make the graph. The route through the triangles' shared edges is then shortened: a
waypoint is dropped while the straight line from the previous kept one stays on walkable ground.

usage: python navmesh.py <module, e.g. m01ab> <x0> <y0> <x1> <y1>    -> "x y x y ..." for `game goto`
       python navmesh.py <module> info                                  -> rooms, triangles, bounds
"""
import heapq
import math
import struct
import sys

import kres

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\swkotor"
kres.TYPES.update({"lyt": 3000, "wok": 2016})


def walkable_materials():
    cols, _labels, rows = kres.read_2da(kres.extract(GAME, "surfacemat", "2da"))
    wi = cols.index("walk")
    return {i for i, r in enumerate(rows) if r[wi] == "1"}


def load(module):
    lyt = kres.extract(GAME, module, "lyt").decode("latin1").splitlines()
    rooms, inrooms = [], False
    for line in lyt:
        t = line.split()
        if not t:
            continue
        if t[0] == "roomcount":
            inrooms, left = True, int(t[1])
            continue
        if inrooms and left > 0:
            rooms.append((t[0].lower(), float(t[1]), float(t[2]), float(t[3])))
            left -= 1
            inrooms = left > 0
    walk = walkable_materials()
    tris = []  # (a, b, c) vertices (x, y, z)
    for name, rx, ry, rz in rooms:
        try:
            d = kres.extract(GAME, name, "wok")
        except KeyError:
            continue
        nv, ov, nf, of, om = struct.unpack_from("<5I", d, 72)
        verts = [struct.unpack_from("<3f", d, ov + i * 12) for i in range(nv)]
        # room walkmeshes are stored in world coordinates when the room sits at its layout position; some are local
        local = all(abs(v[0]) < 60 and abs(v[1]) < 60 for v in verts) and (abs(rx) > 60 or abs(ry) > 60)
        if local:
            verts = [(v[0] + rx, v[1] + ry, v[2] + rz) for v in verts]
        for f in range(nf):
            a, b, c = struct.unpack_from("<3I", d, of + f * 12)
            mat = struct.unpack_from("<I", d, om + f * 4)[0]
            if mat in walk:
                tris.append((verts[a], verts[b], verts[c]))
    return rooms, tris


def key(v):
    return (round(v[0], 2), round(v[1], 2))


def build(tris):
    edges = {}
    for i, t in enumerate(tris):
        for e in range(3):
            k = tuple(sorted((key(t[e]), key(t[(e + 1) % 3]))))
            edges.setdefault(k, []).append(i)
    adj = [[] for _ in tris]
    for k, ts in edges.items():
        for i in ts:
            for j in ts:
                if i != j:
                    adj[i].append((j, k))
    return adj


def inside(t, x, y):
    (ax, ay, _), (bx, by, _), (cx, cy, _) = t
    d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
    d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
    d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
    neg = d1 < -1e-6 or d2 < -1e-6 or d3 < -1e-6
    pos = d1 > 1e-6 or d2 > 1e-6 or d3 > 1e-6
    return not (neg and pos)


def tri_at(tris, x, y):
    for i, t in enumerate(tris):
        if inside(t, x, y):
            return i
    # nearest centroid (a point just off the mesh, e.g. a door's plane)
    return min(range(len(tris)), key=lambda i: (sum(v[0] for v in tris[i]) / 3 - x) ** 2 + (sum(v[1] for v in tris[i]) / 3 - y) ** 2)


def walkable_line(tris, p, q, step=0.25, margin=0.35):
    """The segment and a margin either side of it stay on walkable triangles (no corner clipping)."""
    n = max(2, int(math.dist(p, q) / step))
    dx, dy = q[0] - p[0], q[1] - p[1]
    ln = math.hypot(dx, dy) or 1
    nx, ny = -dy / ln * margin, dx / ln * margin
    for i in range(n + 1):
        x, y = p[0] + dx * i / n, p[1] + dy * i / n
        for ox, oy in ((0, 0), (nx, ny), (-nx, -ny)):
            if not any(inside(t, x + ox, y + oy) for t in tris):
                return False
    return True


def route(module, x0, y0, x1, y1):
    _rooms, tris = load(module)
    adj = build(tris)
    s, g = tri_at(tris, x0, y0), tri_at(tris, x1, y1)
    cen = [(sum(v[0] for v in t) / 3, sum(v[1] for v in t) / 3) for t in tris]
    dist, prev, via = {s: 0.0}, {}, {}
    pq = [(math.dist(cen[s], cen[g]), s)]
    while pq:
        _f, u = heapq.heappop(pq)
        if u == g:
            break
        for v, e in adj[u]:
            mid = ((e[0][0] + e[1][0]) / 2, (e[0][1] + e[1][1]) / 2)
            nd = dist[u] + math.dist(cen[u], mid) + math.dist(mid, cen[v])
            if nd < dist.get(v, 1e18):
                dist[v], prev[v], via[v] = nd, u, mid
                heapq.heappush(pq, (nd + math.dist(cen[v], cen[g]), v))
    if g not in dist:
        raise SystemExit("no walkable route")
    pts, u = [(x1, y1)], g
    while u != s:
        pts.append(via[u])
        u = prev[u]
    pts.append((x0, y0))
    pts.reverse()
    out, i = [pts[0]], 0
    while i < len(pts) - 1:
        j = len(pts) - 1
        while j > i + 1 and not walkable_line(tris, pts[i], pts[j]):
            j -= 1
        out.append(pts[j])
        i = j
    return out[1:]


if __name__ == "__main__":
    mod = sys.argv[1]
    if sys.argv[2] == "info":
        rooms, tris = load(mod)
        xs = [v[0] for t in tris for v in t]
        ys = [v[1] for t in tris for v in t]
        print(len(rooms), "rooms", len(tris), "walkable triangles", "x %.1f..%.1f y %.1f..%.1f" % (min(xs), max(xs), min(ys), max(ys)))
    else:
        x0, y0, x1, y1 = map(float, sys.argv[2:6])
        print(" ".join("%.2f %.2f" % p for p in route(mod, x0, y0, x1, y1)))

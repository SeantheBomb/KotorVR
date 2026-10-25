"""Minimal KotOR 1 (PC) binary MDL/MDX reader: node tree, transforms, trimesh geometry, textures.

Offsets inside the MDL are relative to byte 12 (after the 12-byte file header). Layout per the community format
docs (xoreos / PyKotor / KotorBlender): 80-byte node header, 332-byte K1 trimesh header, 32-byte faces.

    python mdl.py <file.mdl> [file.mdx]     # dump the node tree and mesh stats
"""
import struct
import sys

NODE_HEADER, NODE_LIGHT, NODE_EMITTER, NODE_CAMERA, NODE_REFERENCE = 0x1, 0x2, 0x4, 0x8, 0x10
NODE_MESH, NODE_SKIN, NODE_ANIM, NODE_DANGLY, NODE_AABB, NODE_SABER = 0x20, 0x40, 0x80, 0x100, 0x200, 0x800

CTRL_POSITION, CTRL_ORIENTATION = 8, 20


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin1")


class Mesh:
    def __init__(self):
        self.texture = ""
        self.lightmap = ""
        self.positions = []  # (x, y, z) local to the node
        self.normals = []
        self.uvs = []
        self.faces = []      # (a, b, c) vertex indices
        self.render = True
        self.diffuse = (1, 1, 1)
        self.transparency = 0


class Node:
    def __init__(self):
        self.name = ""
        self.type = 0
        self.position = (0.0, 0.0, 0.0)
        self.orientation = (1.0, 0.0, 0.0, 0.0)  # w, x, y, z
        self.children = []
        self.mesh = None


class Model:
    def __init__(self, mdl, mdx):
        self.mdl = mdl
        self.mdx = mdx
        d = mdl[12:]
        self.d = d
        self.name = cstr(d[8:40])
        root_off = struct.unpack_from("<I", d, 40)[0]
        self.bbmin = struct.unpack_from("<3f", d, 104)
        self.bbmax = struct.unpack_from("<3f", d, 116)
        self.radius = struct.unpack_from("<f", d, 128)[0]
        self.supermodel = cstr(d[136:168])
        names_off, names_cnt = struct.unpack_from("<II", d, 184)
        self.names = [cstr(d[o:o + 64]) for o in struct.unpack_from("<%dI" % names_cnt, d, names_off)]
        self.root = self._node(root_off)

    def _array(self, off):
        return struct.unpack_from("<II", self.d, off)  # (offset, count)

    def _node(self, off):
        d = self.d
        n = Node()
        n.type, _idx, name_idx = struct.unpack_from("<HHH", d, off)
        n.name = self.names[name_idx] if name_idx < len(self.names) else "?"
        n.position = struct.unpack_from("<3f", d, off + 16)
        n.orientation = struct.unpack_from("<4f", d, off + 28)
        ch_off, ch_cnt = self._array(off + 44)
        ct_off, ct_cnt = self._array(off + 56)
        cd_off, cd_cnt = self._array(off + 68)
        # static transform: the header holds position + quaternion (w,x,y,z); position/orientation controllers
        # repeat it with values at `dataidx` (times live separately at `timeidx`; quaternions there are x,y,z,w)
        vals = struct.unpack_from("<%df" % cd_cnt, d, cd_off) if cd_cnt else ()
        for i in range(ct_cnt):
            ctype, _u, rows, _ti, di, cols = struct.unpack_from("<IHHHHB", d, ct_off + i * 16)
            if ctype == CTRL_POSITION and cols == 3 and di + 3 <= len(vals):
                n.position = tuple(vals[di:di + 3])
            if ctype == CTRL_ORIENTATION and cols == 4 and di + 4 <= len(vals):
                x, y, z, w = vals[di:di + 4]
                n.orientation = (w, x, y, z)
        if n.type & NODE_MESH:
            n.mesh = self._mesh(off + 80, n.type)
        for co in struct.unpack_from("<%dI" % ch_cnt, d, ch_off):
            n.children.append(self._node(co))
        return n

    def _mesh(self, m, ntype):
        d = self.d
        me = Mesh()
        f_off, f_cnt = self._array(m + 8)
        me.diffuse = struct.unpack_from("<3f", d, m + 60)
        me.transparency = struct.unpack_from("<I", d, m + 84)[0]
        me.texture = cstr(d[m + 88:m + 120])
        me.lightmap = cstr(d[m + 120:m + 152])
        stride, flags = struct.unpack_from("<II", d, m + 252)
        o_vert, o_norm, o_col, o_uv1 = struct.unpack_from("<4i", d, m + 260)
        vcount, _tcount = struct.unpack_from("<HH", d, m + 304)
        me.render = d[m + 313] != 0
        mdx_off, vcoord_off = struct.unpack_from("<II", d, m + 324)
        for i in range(vcount):
            base = mdx_off + i * stride
            if o_vert >= 0 and base + o_vert + 12 <= len(self.mdx):
                me.positions.append(struct.unpack_from("<3f", self.mdx, base + o_vert))
            else:
                me.positions.append(struct.unpack_from("<3f", d, vcoord_off + i * 12))
            me.normals.append(struct.unpack_from("<3f", self.mdx, base + o_norm) if o_norm >= 0 else (0, 0, 1))
            me.uvs.append(struct.unpack_from("<2f", self.mdx, base + o_uv1) if o_uv1 >= 0 else (0, 0))
        for i in range(f_cnt):
            a, b, c = struct.unpack_from("<3H", d, f_off + i * 32 + 26)
            if a < vcount and b < vcount and c < vcount:
                me.faces.append((a, b, c))
        return me

    def walk(self, node=None, depth=0):
        node = node or self.root
        yield node, depth
        for c in node.children:
            yield from self.walk(c, depth + 1)


def types(t):
    names = [(NODE_MESH, "mesh"), (NODE_SKIN, "skin"), (NODE_DANGLY, "dangly"), (NODE_AABB, "aabb"),
             (NODE_SABER, "saber"), (NODE_EMITTER, "emitter"), (NODE_LIGHT, "light"), (NODE_REFERENCE, "ref")]
    return "+".join(n for f, n in names if t & f) or "dummy"


if __name__ == "__main__":
    mdl = open(sys.argv[1], "rb").read()
    mdx = open(sys.argv[2] if len(sys.argv) > 2 else sys.argv[1][:-1] + "x", "rb").read()
    m = Model(mdl, mdx)
    print(f"model {m.name} super '{m.supermodel}' bb {m.bbmin} - {m.bbmax} r {m.radius:.3f}")
    for n, depth in m.walk():
        line = f"{'  ' * depth}{n.name} [{types(n.type)}] pos ({n.position[0]:.3f},{n.position[1]:.3f},{n.position[2]:.3f}) " \
               f"q ({n.orientation[0]:.2f},{n.orientation[1]:.2f},{n.orientation[2]:.2f},{n.orientation[3]:.2f})"
        if n.mesh:
            me = n.mesh
            xs = [p[0] for p in me.positions] or [0]
            ys = [p[1] for p in me.positions] or [0]
            zs = [p[2] for p in me.positions] or [0]
            line += f" | {len(me.positions)} v {len(me.faces)} f tex '{me.texture}' render {me.render} " \
                    f"x[{min(xs):.3f},{max(xs):.3f}] y[{min(ys):.3f},{max(ys):.3f}] z[{min(zs):.3f},{max(zs):.3f}]"
        print(line)

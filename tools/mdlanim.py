"""KotOR 1 (PC) binary MDL/MDX reader with animations (copied from the Kotor Gate converter, tools/kotor/mdl.py).

All offsets inside the MDL are relative to byte 12 (after the 12-byte file header).
Layout per the community format docs (mdlops, kotorblender, xoreos, reone) and the KotorVR reader:
  geometry header 80 bytes, model header to 196, node header 80, light 92, emitter 224, reference 36,
  trimesh 332, skin 100, dangly 28, aabb 4, saber 20, face 32, controller key 16, animation event 36.
Coordinates are returned exactly as stored (Z up); conversion happens in the exporter.
"""
import struct

import numpy as np

NODE_HEADER, NODE_LIGHT, NODE_EMITTER, NODE_CAMERA, NODE_REFERENCE = 0x1, 0x2, 0x4, 0x8, 0x10
NODE_MESH, NODE_SKIN, NODE_ANIM, NODE_DANGLY, NODE_AABB, NODE_SABER = 0x20, 0x40, 0x80, 0x100, 0x200, 0x800
TYPE_NAMES = [(NODE_LIGHT, "light"), (NODE_EMITTER, "emitter"), (NODE_CAMERA, "camera"), (NODE_REFERENCE, "reference"),
              (NODE_MESH, "mesh"), (NODE_SKIN, "skin"), (NODE_ANIM, "animmesh"), (NODE_DANGLY, "dangly"),
              (NODE_AABB, "aabb"), (NODE_SABER, "saber")]

CTRL_POSITION, CTRL_ORIENTATION, CTRL_SCALE = 8, 20, 36
# Controller ids whose meaning is certain. Everything else is exported as "ctrl_<id>".
CTRL_NAMES_COMMON = {8: "position", 20: "orientation", 36: "scale"}
CTRL_NAMES_MESH = {100: "selfillumcolor", 132: "alpha"}
CTRL_NAMES_LIGHT = {76: "color", 88: "radius", 96: "shadowradius", 100: "verticaldisplacement", 140: "multiplier"}

# Emitter controller ids, per the kotorblender / mdlops tables. The ids seen in the game's animations
# (80, 84, 88, 120..176, 216..240, 284, 380, 392) all fall on these and have the expected column counts.
CTRL_NAMES_EMITTER = {
    80: "alphaend", 84: "alphastart", 88: "birthrate", 92: "bounce_co", 96: "combinetime", 100: "drag", 104: "fps",
    108: "frameend", 112: "framestart", 116: "grav", 120: "lifeexp", 124: "mass", 128: "p2p_bezier2", 132: "p2p_bezier3",
    136: "particlerot", 140: "randvel", 144: "sizestart", 148: "sizeend", 152: "sizestart_y", 156: "sizeend_y",
    160: "spread", 164: "threshold", 168: "velocity", 172: "xsize", 176: "ysize", 180: "blurlength",
    184: "lightningdelay", 188: "lightningradius", 192: "lightningscale", 196: "lightningsubdiv", 200: "lightningzigzag",
    216: "alphamid", 220: "percentstart", 224: "percentmid", 228: "percentend", 232: "sizemid", 236: "sizemid_y",
    240: "randombirthrate", 252: "targetsize", 256: "numcontrolpts", 260: "controlptradius", 264: "controlptdelay",
    268: "tangentspread", 272: "tangentlength", 284: "colormid", 380: "colorend", 392: "colorstart", 502: "detonate",
}

K1_PC_FNPTR = (4273776, 4273392)  # model / animation-mesh variants of the first function pointer


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin1")


class Controller:
    __slots__ = ("type", "times", "values", "bezier", "compressed", "raw")


class Node:
    def __init__(self):
        self.children = []
        self.controllers = []
        self.mesh = None
        self.extra = {}

    def type_names(self):
        return [n for f, n in TYPE_NAMES if self.type & f] or ["dummy"]

    def ctrl(self, ctype):
        for c in self.controllers:
            if c.type == ctype:
                return c
        return None


class Mesh:
    pass


class Animation:
    pass


class Model:
    def __init__(self, mdl, mdx):
        if struct.unpack_from("<I", mdl, 0)[0] != 0:
            raise ValueError("not a binary MDL (ASCII model?)")
        self.d = d = memoryview(mdl)[12:]
        self.mdx = mdx
        self.fnptr = struct.unpack_from("<II", d, 0)
        self.name = cstr(bytes(d[8:40]))
        root_off, self.node_count = struct.unpack_from("<II", d, 40)
        self.geometry_type = d[76]
        self.classification, self.subclassification, _unk, self.affected_by_fog = struct.unpack_from("<4B", d, 80)
        anim_off, anim_cnt = struct.unpack_from("<II", d, 88)
        self.bbmin = struct.unpack_from("<3f", d, 104)
        self.bbmax = struct.unpack_from("<3f", d, 116)
        self.radius, self.animation_scale = struct.unpack_from("<ff", d, 128)
        self.supermodel = cstr(bytes(d[136:168]))
        names_off, names_cnt = struct.unpack_from("<II", d, 184)
        self.names = [cstr(bytes(d[o:o + 64])) for o in struct.unpack_from("<%dI" % names_cnt, d, names_off)]
        self.nodes = []  # geometry nodes in depth-first order
        self.root = self._node(root_off, None, self.nodes, True)
        self.animations = []
        for ao in struct.unpack_from("<%dI" % anim_cnt, d, anim_off):
            self.animations.append(self._animation(ao))

    # ---- nodes -------------------------------------------------------------------------------------------------
    def _controllers(self, off, ntype):
        d = self.d
        ct_off, ct_cnt = struct.unpack_from("<II", d, off + 56)
        cd_off, cd_cnt = struct.unpack_from("<II", d, off + 68)
        if not ct_cnt:
            return []
        if cd_off + cd_cnt * 4 > len(d):
            raise ValueError("controller data runs past end of file")
        data = np.frombuffer(d, dtype="<f4", count=cd_cnt, offset=cd_off)
        out = []
        for i in range(ct_cnt):
            ctype, _unk, rows, ti, di, cols = struct.unpack_from("<IHHHHB", d, ct_off + i * 16)
            c = Controller()
            c.type = ctype
            c.bezier = bool(cols & 16)
            c.compressed = False
            n = cols & 15
            c.times = data[ti:ti + rows].copy()
            if ctype == CTRL_ORIENTATION and n == 2:
                # 11/11/10-bit packed x, y, z; w is recovered as the positive root. Sign convention checked
                # against rest poses: this form gives a 21 degree median difference on c_rancor's idle,
                # the conjugate gives 96.
                raw = data[di:di + rows].view("<u4")
                x = (raw & 0x7FF).astype(np.float64) / 1023.0 - 1.0
                y = ((raw >> 11) & 0x7FF).astype(np.float64) / 1023.0 - 1.0
                z = (raw >> 22).astype(np.float64) / 511.0 - 1.0
                m2 = x * x + y * y + z * z
                w = np.sqrt(np.clip(1.0 - m2, 0.0, None))
                q = np.stack([x, y, z, w], axis=1)
                q /= np.linalg.norm(q, axis=1, keepdims=True)
                c.values = q.astype(np.float32)
                c.compressed = True
            else:
                width = n * 3 if c.bezier else n
                if di + rows * width > cd_cnt:
                    raise ValueError("controller %d data out of range" % ctype)
                full = data[di:di + rows * width].reshape(rows, width) if n else np.zeros((rows, 0), "<f4")
                c.values = full[:, :n].copy()
                if c.bezier:
                    c.raw = full.copy()  # value, then the two bezier control points, per key
            if len(c.times) != rows:
                raise ValueError("controller %d times out of range" % ctype)
            out.append(c)
        return out

    def _node(self, off, parent, flat, geometry):
        d = self.d
        n = Node()
        n.type, n.supernode, n.name_index = struct.unpack_from("<HHH", d, off)
        n.name = self.names[n.name_index] if n.name_index < len(self.names) else "node%d" % n.name_index
        n.parent = parent
        n.position = struct.unpack_from("<3f", d, off + 16)
        w, x, y, z = struct.unpack_from("<4f", d, off + 28)
        n.orientation = (x, y, z, w)
        n.controllers = self._controllers(off, n.type)
        flat.append(n)
        p = off + 80
        if n.type & NODE_LIGHT:
            if geometry:
                flare_radius = struct.unpack_from("<f", d, p)[0]
                ints = struct.unpack_from("<7I", d, p + 64)
                n.extra["light"] = dict(zip(("priority", "ambientonly", "dynamictype", "affectdynamic", "shadow", "flare",
                                             "fadinglight"), ints), flareradius=flare_radius, raw=bytes(d[p:p + 92]).hex())
            p += 92
        if n.type & NODE_EMITTER:
            if geometry:
                dead, blast_r, blast_l, branch, smooth, xgrid, ygrid, spawn = struct.unpack_from("<3fIf3I", d, p)
                two_sided, loop, order, frame_blend = struct.unpack_from("<IIHB", d, p + 176)
                n.extra["emitter"] = {
                    "deadspace": dead, "blastradius": blast_r, "blastlength": blast_l, "numbranches": branch,
                    "controlptsmoothing": smooth, "xgrid": xgrid, "ygrid": ygrid, "spawntype": spawn,
                    "update": cstr(bytes(d[p + 32:p + 64])), "render": cstr(bytes(d[p + 64:p + 96])),
                    "blend": cstr(bytes(d[p + 96:p + 128])), "texture": cstr(bytes(d[p + 128:p + 160])),
                    "chunkname": cstr(bytes(d[p + 160:p + 176])), "twosidedtex": two_sided, "loop": loop,
                    "renderorder": order, "frameblending": frame_blend,
                    "depthtexture": cstr(bytes(d[p + 187:p + 219])), "flags": struct.unpack_from("<I", d, p + 220)[0],
                    "raw": bytes(d[p:p + 224]).hex(),
                }
            p += 224
        if n.type & NODE_REFERENCE:
            if geometry:
                n.extra["reference"] = {"model": cstr(bytes(d[p:p + 32])), "reattachable": struct.unpack_from("<I", d, p + 32)[0]}
            p += 36
        if n.type & NODE_MESH:
            if geometry:
                n.mesh = self._mesh(p, n)
            p += 332
            if geometry:
                if n.type & NODE_DANGLY:
                    c_off, c_cnt = struct.unpack_from("<II", d, p)
                    disp, tight, period = struct.unpack_from("<3f", d, p + 12)
                    n.extra["dangly"] = {"displacement": disp, "tightness": tight, "period": period,
                                         "constraints": [float(v) for v in np.frombuffer(d, "<f4", c_cnt, c_off)] if c_cnt else []}
                if n.type & NODE_SABER:
                    n.extra["saber"] = {"raw": bytes(d[p:p + 20]).hex()}
        ch_off, ch_cnt = struct.unpack_from("<II", d, off + 44)
        if ch_cnt > 4096:
            raise ValueError("implausible child count %d" % ch_cnt)
        for co in struct.unpack_from("<%dI" % ch_cnt, d, ch_off):
            n.children.append(self._node(co, n, flat, geometry))
        return n

    def _mesh(self, m, node):
        d, mdx = self.d, self.mdx
        me = Mesh()
        f_off, f_cnt = struct.unpack_from("<II", d, m + 8)
        me.diffuse = struct.unpack_from("<3f", d, m + 60)
        me.ambient = struct.unpack_from("<3f", d, m + 72)
        me.transparency_hint = struct.unpack_from("<I", d, m + 84)[0]
        me.texture = cstr(bytes(d[m + 88:m + 120])).lower()
        me.lightmap = cstr(bytes(d[m + 120:m + 152])).lower()
        me.animate_uv, me.uv_dir_x, me.uv_dir_y, me.uv_jitter, me.uv_jitter_speed = struct.unpack_from("<I4f", d, m + 232)
        stride, me.mdx_flags = struct.unpack_from("<II", d, m + 252)
        o_vert, o_norm, _o_col, o_uv1, o_uv2 = struct.unpack_from("<5i", d, m + 260)
        vcount, me.texture_count = struct.unpack_from("<HH", d, m + 304)
        (me.has_lightmap, me.rotate_texture, me.background_geometry, me.shadow, me.beaming,
         me.render) = struct.unpack_from("<6B", d, m + 308)
        mdx_off, vcoord_off = struct.unpack_from("<II", d, m + 324)
        me.vertex_count = vcount
        me.normals = me.uv1 = me.uv2 = me.weights = me.bone_slots = None
        me.bonemap = None
        rows = None
        if vcount and stride and mdx_off + vcount * stride <= len(mdx):
            rows = np.frombuffer(mdx, dtype=np.uint8, count=vcount * stride, offset=mdx_off).reshape(vcount, stride)

        def col(o, n):
            if rows is None or o < 0 or o + n * 4 > stride:
                return None
            return rows[:, o:o + n * 4].copy().view("<f4").reshape(vcount, n)

        me.positions = col(o_vert, 3)
        if me.positions is None:
            if vcount and vcoord_off + vcount * 12 <= len(d):
                me.positions = np.frombuffer(d, "<f4", vcount * 3, vcoord_off).reshape(vcount, 3).copy()
            else:
                me.positions = np.zeros((0, 3), "<f4")
        me.normals = col(o_norm, 3)
        me.uv1 = col(o_uv1, 2)
        me.uv2 = col(o_uv2, 2)
        if f_cnt:
            if f_off + f_cnt * 32 > len(d):
                raise ValueError("face array runs past end of file")
            faces = np.frombuffer(d, dtype=np.uint8, count=f_cnt * 32, offset=f_off).reshape(f_cnt, 32)
            idx = faces[:, 26:32].copy().view("<u2").reshape(f_cnt, 3)
            idx = idx[(idx < len(me.positions)).all(axis=1)]
            me.faces = idx
        else:
            me.faces = np.zeros((0, 3), "<u2")
        if node.type & NODE_SKIN:
            s = m + 332
            o_w, o_i, bm_off, bm_cnt = struct.unpack_from("<4I", d, s + 12)
            qb_off, qb_cnt = struct.unpack_from("<II", d, s + 28)
            tb_off, tb_cnt = struct.unpack_from("<II", d, s + 40)
            me.bonemap = np.frombuffer(d, "<f4", bm_cnt, bm_off).copy() if bm_cnt else np.zeros(0, "<f4")
            me.qbones = np.frombuffer(d, "<f4", qb_cnt * 4, qb_off).reshape(qb_cnt, 4).copy() if qb_cnt else None  # w,x,y,z
            me.tbones = np.frombuffer(d, "<f4", tb_cnt * 3, tb_off).reshape(tb_cnt, 3).copy() if tb_cnt else None
            me.weights = col(o_w, 4)
            me.bone_slots = col(o_i, 4)
        return me

    # ---- animations --------------------------------------------------------------------------------------------
    def _animation(self, off):
        d = self.d
        a = Animation()
        a.name = cstr(bytes(d[off + 8:off + 40]))
        root_off = struct.unpack_from("<I", d, off + 40)[0]
        a.length, a.transtime = struct.unpack_from("<ff", d, off + 80)
        a.animroot = cstr(bytes(d[off + 88:off + 120]))
        ev_off, ev_cnt = struct.unpack_from("<II", d, off + 120)
        a.events = []
        for i in range(ev_cnt):
            o = ev_off + i * 36
            a.events.append((struct.unpack_from("<f", d, o)[0], cstr(bytes(d[o + 4:o + 36]))))
        a.nodes = []
        self._node(root_off, None, a.nodes, False)
        return a

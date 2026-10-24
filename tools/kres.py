"""Minimal KotOR resource tool: extract from chitin.key/BIF, read/write binary 2DA (V2.b).

usage:
  python kres.py <game_dir> extract <resref> <ext> <out_file>
  python kres.py <game_dir> show2da <resref>
"""
import os
import struct
import sys

TYPES = {"2da": 2017, "gff": 2037, "dlg": 2029, "utc": 2027, "mdl": 2002, "mdx": 3008, "ncs": 2010, "nss": 2009}


def read_key(game):
    d = open(os.path.join(game, "chitin.key"), "rb").read()
    assert d[:8] == b"KEY V1  "
    nbif, nkey, off_file, off_key = struct.unpack_from("<4I", d, 8)
    bifs = []
    for i in range(nbif):
        size, name_off, name_len, drives = struct.unpack_from("<IIHH", d, off_file + i * 12)
        bifs.append(d[name_off:name_off + name_len].rstrip(b"\0").decode().replace("\\", os.sep))
    keys = {}
    for i in range(nkey):
        o = off_key + i * 22
        resref = d[o:o + 16].rstrip(b"\0").decode().lower()
        rtype, rid = struct.unpack_from("<HI", d, o + 16)
        keys[(resref, rtype)] = rid
    return bifs, keys


def extract(game, resref, ext):
    bifs, keys = read_key(game)
    rid = keys[(resref.lower(), TYPES[ext])]
    bif = open(os.path.join(game, bifs[rid >> 20]), "rb").read()
    assert bif[:8] == b"BIFFV1  "
    nvar, nfix, off_var = struct.unpack_from("<3I", bif, 8)
    idx = rid & 0xFFFFF
    _id, off, size, _t = struct.unpack_from("<4I", bif, off_var + idx * 16)
    return bif[off:off + size]


def read_2da(data):
    assert data[:9] == b"2DA V2.b\n", data[:9]
    p = 9
    end = data.index(b"\0", p)
    cols = data[p:end].decode().split("\t")[:-1]
    p = end + 1
    (nrows,) = struct.unpack_from("<I", data, p)
    p += 4
    labels = []
    for _ in range(nrows):
        e = data.index(b"\t", p)
        labels.append(data[p:e].decode())
        p = e + 1
    n = nrows * len(cols)
    offs = struct.unpack_from("<%dH" % n, data, p)
    p += 2 * n
    (dsize,) = struct.unpack_from("<H", data, p)
    p += 2
    blob = data[p:p + dsize]
    rows = []
    for r in range(nrows):
        row = []
        for c in range(len(cols)):
            o = offs[r * len(cols) + c]
            row.append(blob[o:blob.index(b"\0", o)].decode())
        rows.append(row)
    return cols, labels, rows


def write_2da(cols, labels, rows):
    out = bytearray(b"2DA V2.b\n")
    out += ("\t".join(cols) + "\t").encode() + b"\0"
    out += struct.pack("<I", len(rows))
    for l in labels:
        out += l.encode() + b"\t"
    blob = bytearray()
    index = {}
    offs = []
    for row in rows:
        for v in row:
            if v not in index:
                index[v] = len(blob)
                blob += v.encode() + b"\0"
            offs.append(index[v])
    out += struct.pack("<%dH" % len(offs), *offs)
    out += struct.pack("<H", len(blob))
    out += blob
    return bytes(out)


if __name__ == "__main__":
    game, cmd = sys.argv[1], sys.argv[2]
    if cmd == "extract":
        open(sys.argv[5], "wb").write(extract(game, sys.argv[3], sys.argv[4]))
    elif cmd == "show2da":
        cols, labels, rows = read_2da(extract(game, sys.argv[3], "2da"))
        print("\t".join(["#"] + cols))
        for l, r in zip(labels, rows):
            print("\t".join([l] + r))

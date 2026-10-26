"""Find the start of the function containing an address (padding scan) and all direct E8 calls / pointer refs to it,
in the probe's image dump.  usage: python callers.py <hex addr>..."""
import struct, sys
d = open(r'C:\Program Files (x86)\Steam\steamapps\common\swkotor\kvr\image_00400000.bin', 'rb').read()
B = 0x400000
TEXT_END = 0x734000


def func_start(a):
    for x in range(a, a - 0x4000, -1):
        if d[x - B - 1] in (0xCC, 0x90, 0xC3) and d[x - B - 2] in (0xCC, 0x90, 0xC3, 0xC2) and d[x - B] not in (0xCC, 0x90):
            if x % 16 == 0 or d[x - B - 1] == 0xCC:
                return x
    return None


def callers(t):
    out = []
    for o in range(0x1000, TEXT_END - B - 5):
        if d[o] == 0xE8 and o + B + 5 + struct.unpack_from('<i', d, o + 1)[0] == t:
            out.append(o + B)
    pat = struct.pack('<I', t)
    i = d.find(pat)
    refs = []
    while i >= 0:
        refs.append(i + B)
        i = d.find(pat, i + 1)
    return out, refs


for arg in sys.argv[1:]:
    a = int(arg, 16)
    s = func_start(a)
    c, r = callers(s) if s else ([], [])
    print('%x: function %s, calls from %s, refs %s' % (a, hex(s) if s else '?', [hex(x) for x in c], [hex(x) for x in r]))

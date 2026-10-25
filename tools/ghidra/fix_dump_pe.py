"""Turn the probe's in-memory image dump (kvr/image_00400000.bin) into a PE whose sections' file offsets equal their
RVAs, so Ghidra's PE loader maps it correctly (decrypted .text, import names, entry point).
usage: python fix_dump_pe.py <dump.bin> <out.exe>"""
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
pe = struct.unpack_from('<I', d, 0x3C)[0]
assert d[pe:pe + 4] == b'PE\0\0'
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
sec = pe + 24 + opt
for i in range(nsec):
    o = sec + i * 40
    name = d[o:o + 8].rstrip(b'\0').decode()
    vsize, va = struct.unpack_from('<II', d, o + 8)
    vsize = min(vsize, len(d) - va) if va < len(d) else 0
    struct.pack_into('<II', d, o + 16, vsize, va)  # SizeOfRawData, PointerToRawData
    print(f'{name:8s} va {va:08X} size {vsize:08X}')
open(sys.argv[2], 'wb').write(d)

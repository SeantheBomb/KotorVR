"""Disassembly helpers over a dumped swkotor.exe image.

usage:
  python disasm.py <image.bin> dis <va_hex> [count]     linear disassembly
  python disasm.py <image.bin> callers <va_hex>         direct call/jmp sites targeting va
  python disasm.py <image.bin> calls <va_hex> [count]   calls made from a function
  python disasm.py <image.bin> strings <regex>          printable strings matching regex
"""
import re
import struct
import sys

import capstone
import pefile

BASE = 0x400000


def load(path):
    img = open(path, "rb").read()
    pe = pefile.PE(data=img, fast_load=True)
    t = next(s for s in pe.sections if s.Name.startswith(b".text"))
    return img, t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize


def string_at(img, va):
    off = va - BASE
    if not (0 <= off < len(img)):
        return None
    e = img.find(b"\0", off, off + 200)
    s = img[off:e] if e > off else b""
    return s.decode("latin1") if len(s) >= 4 and all(0x20 <= c < 0x7F or c in (9, 10) for c in s) else None


def dis(img, va, count):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for n, i in enumerate(md.disasm(img[va - BASE: va - BASE + count * 16], va)):
        if n >= count:
            break
        note = ""
        for m in re.finditer(r"0x([0-9a-f]{6,8})", i.op_str):
            s = string_at(img, int(m.group(1), 16))
            if s:
                note = f"   ; {s[:70]!r}"
        print(f"{i.address:08X}  {i.mnemonic:7} {i.op_str}{note}")
        if i.mnemonic == "ret" and count > 400:
            break


def callers(img, t0, t1, va):
    code = img[t0:t1]
    for op in (0xE8, 0xE9):
        for i in range(len(code) - 5):
            if code[i] == op:
                rel = struct.unpack_from("<i", code, i + 1)[0]
                src = BASE + t0 + i
                if src + 5 + rel == va:
                    print(f"{'call' if op == 0xE8 else 'jmp '} from {src:08X}")


def calls(img, va, count):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    seen = []
    for n, i in enumerate(md.disasm(img[va - BASE: va - BASE + count * 16], va)):
        if n >= count:
            break
        if i.mnemonic == "call":
            print(f"{i.address:08X}  call {i.op_str}")


def strings(img, pat):
    rx = re.compile(pat.encode(), re.I)
    for m in re.finditer(rb"[ -~]{5,}", img):
        if rx.search(m.group()):
            print(f"{BASE + m.start():08X}  {m.group().decode()}")


if __name__ == "__main__":
    img, t0, t1 = load(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "dis":
        dis(img, int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 60)
    elif cmd == "callers":
        callers(img, t0, t1, int(sys.argv[3], 16))
    elif cmd == "calls":
        calls(img, int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 400)
    elif cmd == "strings":
        strings(img, sys.argv[3])

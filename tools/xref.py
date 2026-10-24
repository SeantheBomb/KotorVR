"""Find code references to strings in a dumped swkotor.exe image.

usage: python -I xref.py <image.bin> [base_hex] [pattern ...]
Prints each matching string, every instruction in .text that loads its address,
and a best-guess start of the containing function.
"""
import re
import struct
import sys

import capstone
import pefile

DEFAULT_PATTERNS = [
    rb"\(Base Attack Bonus\)",
    rb"Damage Roll: %d",
    rb"AttackDebugText",
    rb"CSWSCombatRound::IncrementTimer",
    rb"CSWSCombatRound::EndCombatRound",
    rb"RoundLength",
    rb"AttackResult",
    rb"AttackDeflected",
    rb"CAMERAHOOK",
    rb"CameraViewAngle",
]


def main():
    img_path = sys.argv[1]
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x400000
    pats = [p.encode() for p in sys.argv[3:]] or DEFAULT_PATTERNS
    img = open(img_path, "rb").read()
    pe = pefile.PE(data=img, fast_load=True)
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
    code = img[t0:t1]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    for pat in pats:
        for m in re.finditer(pat, img):
            # walk back to the start of the C string
            s = m.start()
            while s > 0 and 0x20 <= img[s - 1] < 0x7F:
                s -= 1
            e = img.index(b"\0", m.end())
            va = base + s
            text_str = img[s:e].decode("latin1")
            needle = struct.pack("<I", va)
            refs = [base + t0 + r.start() for r in re.finditer(re.escape(needle), code)]
            print(f"\n{va:08X} {text_str!r}  ({len(refs)} refs)")
            for ref in refs[:12]:
                func = guess_func_start(img, base, ref - base, t0)
                insn = next(md.disasm(img[ref - base - 4: ref - base + 8], ref - 4), None)
                ins = f"{insn.mnemonic} {insn.op_str}" if insn else "?"
                print(f"   ref @ {ref:08X}  in func ~{func:08X}   {ins}")


def guess_func_start(img, base, off, t0):
    """Scan backwards for int3/nop padding followed by a plausible prologue."""
    i = off
    while i > t0 + 1:
        if img[i - 1] in (0xCC, 0x90) and img[i] not in (0xCC, 0x90):
            if img[i] in (0x55, 0x83, 0x81, 0x56, 0x53, 0x57, 0x51, 0x6A, 0x64, 0x8B, 0xA1, 0xB8):
                return base + i
        if img[i - 1] == 0xC3 and img[i] == 0x55:
            return base + i
        i -= 1
    return base + off


if __name__ == "__main__":
    main()

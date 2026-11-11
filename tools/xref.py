"""Find code references to a string (or VA) in a 32-bit PE and disassemble around them.

Usage: python -I xref.py <pe> <string-or-0xVA> [--before=N] [--after=N] [--wide]
Matches absolute 32-bit immediates/displacements equal to the target VA (works for
non-ASLR images and for DLLs at their preferred base, since we never relocate).
"""
import re
import struct
import sys

import capstone
import pefile


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
    before, after = int(opts.get("before", 48)), int(opts.get("after", 96))
    pe = pefile.PE(args[0], fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()
    target = args[1]
    if target.startswith("0x"):
        vas = [int(target, 16)]
    else:
        needle = target.encode("utf-16le") if "wide" in opts else target.encode()
        vas = []
        for m in re.finditer(re.escape(needle), data):
            # walk back to the start of the C string
            start = m.start()
            while start > 0 and 0x20 <= data[start - 1] < 0x7f and "wide" not in opts:
                start -= 1
            vas.append(base + start)
        vas = sorted(set(vas))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    text = [s for s in pe.sections if s.Characteristics & 0x20000000]
    for va in vas:
        s = data[va - base:va - base + 80].split(b"\0")[0]
        print(f"### target {hex(va)} = {s[:70]!r}")
        packed = struct.pack("<I", va)
        for sec in text:
            lo, hi = sec.VirtualAddress, sec.VirtualAddress + sec.Misc_VirtualSize
            for m in re.finditer(re.escape(packed), data[lo:hi]):
                ref = base + lo + m.start()
                start = ref - before
                code = data[start - base:ref - base + after]
                print(f"  -- ref at {hex(ref)} (section {sec.Name.rstrip(b'\0').decode()})")
                # resync: disassemble from several offsets, keep the one that hits ref's instruction
                for skew in range(0, 16):
                    insns = list(md.disasm(code[skew:], start + skew))
                    if any(i.address <= ref < i.address + i.size for i in insns):
                        break
                for i in insns:
                    mark = "=>" if i.address <= ref < i.address + i.size else "  "
                    print(f"     {mark} {i.address:08x}: {i.mnemonic:8s} {i.op_str}")


if __name__ == "__main__":
    main()

"""Find who calls/jumps to (rel32) or points at (abs32, e.g. vtables) an address, with the enclosing function start.

Usage: python -I callers.py <pe> <0xVA> [<0xVA> ...]
"""
import re
import struct
import sys

import pefile


def func_start(img, base, va):
    o = va - base
    while o > 2 and not (img[o - 1] == 0xCC and img[o - 2] == 0xCC):
        o -= 1
    return base + o


def main():
    pe = pefile.PE(sys.argv[1], fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    secs = [s for s in pe.sections]
    for a in sys.argv[2:]:
        target = int(a, 16)
        print(f"### {target:#x}")
        for s in secs:
            name = s.Name.rstrip(b"\0").decode()
            lo = s.VirtualAddress
            data = img[lo:lo + s.Misc_VirtualSize]
            if s.Characteristics & 0x20000000:
                for m in re.finditer(rb"[\xe8\xe9]", data):
                    o = m.start()
                    if o + 5 > len(data):
                        break
                    if base + lo + o + 5 + struct.unpack_from("<i", data, o + 1)[0] == target:
                        site = base + lo + o
                        kind = "call" if data[o] == 0xE8 else "jmp"
                        print(f"  {kind} from {site:#x} in func {func_start(img, base, site):#x} ({name})")
            for m in re.finditer(re.escape(struct.pack("<I", target)), data):
                site = base + lo + m.start()
                where = f"in func {func_start(img, base, site):#x}" if s.Characteristics & 0x20000000 else ""
                print(f"  abs ref at {site:#x} ({name}) {where}")


if __name__ == "__main__":
    main()

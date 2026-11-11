"""Find the vtables of reflected classes: getType functions (`mov eax, <TypeInfo>; ret`) and the vtables whose slot 0
points at one, then list each vtable's entries.

Usage: python classvt.py <pe> <ClassName|0xTypeInfo> [...] [--slots=N] [--types=re/sdk/types.json]
"""
import json
import re
import struct
import sys

import pefile


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    slots = int(opts.get("slots", "48"))
    pe = pefile.PE(args[0], fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    types = {t["name"]: t for t in json.load(open(opts.get("types", "re/sdk/types.json")))}
    text = [s for s in pe.sections if s.Characteristics & 0x20000000]
    data = [s for s in pe.sections if not s.Characteristics & 0x20000000]
    for a in args[1:]:
        ti = int(a, 16) if a.startswith("0x") else types[a]["typeinfo"]
        print(f"### {a} TypeInfo {ti:#x}")
        getters = []
        pat = re.escape(b"\xb8" + struct.pack("<I", ti) + b"\xc3")
        for s in text:
            lo = s.VirtualAddress
            for m in re.finditer(pat, img[lo:lo + s.Misc_VirtualSize]):
                getters.append(base + lo + m.start())
        for g in getters:
            print(f"  getType {g:#x}")
            for s in data:
                lo = s.VirtualAddress
                for m in re.finditer(re.escape(struct.pack("<I", g)), img[lo:lo + s.Misc_VirtualSize]):
                    vt = base + lo + m.start()
                    entries = struct.unpack_from(f"<{slots}I", img, vt - base)
                    print(f"  vtable {vt:#x}:")
                    for i, e in enumerate(entries):
                        print(f"    +{i * 4:#05x} {e:#x}")


if __name__ == "__main__":
    main()

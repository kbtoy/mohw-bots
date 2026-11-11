"""Build a type-hash -> message-name table for every reflected *Message type in one pass.

getType() = `mov eax, <TypeInfo>; ret`; its address sits at slot 0 of the message vtable; the ctor stores
that vtable after `push <typeHash>; push <categoryHash>; call fb::Message::Message`.

Usage: python -I msghash.py <pe> <out.json>
"""
import json
import os
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
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    types = {t["typeinfo"]: t["name"] for t in json.load(open(os.path.join(root, "re", "sdk", "types.json"), encoding="utf-8"))
             if t["name"].endswith("Message")}
    code = [(base + s.VirtualAddress, img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize])
            for s in pe.sections if s.Characteristics & 0x20000000]
    rdata = next(s for s in pe.sections if s.Name.startswith(b".rdata"))
    rlo, rbytes = base + rdata.VirtualAddress, img[rdata.VirtualAddress:rdata.VirtualAddress + rdata.Misc_VirtualSize]

    gettype = {}  # getType addr -> name
    for start, data in code:
        for m in re.finditer(rb"\xb8(.{4})\xc3", data, re.S):
            ti = struct.unpack("<I", m.group(1))[0]
            if ti in types:
                gettype[start + m.start()] = types[ti]
    vtables = {}  # vtable addr -> name
    for o in range(0, len(rbytes) - 3, 4):
        v = struct.unpack_from("<I", rbytes, o)[0]
        if v in gettype:
            vtables[rlo + o] = gettype[v]
    table = {}
    for start, data in code:
        # mov dword ptr [reg], imm32  (C7 00..07 imm32) or mov dword ptr [abs], imm32 (C7 05 abs imm32)
        for m in re.finditer(rb"\xc7([\x00-\x07])(.{4})|\xc7\x05.{4}(.{4})", data, re.S):
            imm = m.group(2) or m.group(3)
            vt = struct.unpack("<I", imm)[0]
            if vt not in vtables:
                continue
            at = start + m.start()
            fs = func_start(img, base, at)
            window = img[fs - base:at - base]
            pushes = [struct.unpack("<I", window[p.start() + 1:p.start() + 5])[0] for p in re.finditer(rb"\x68", window)
                      if p.start() + 5 <= len(window)]
            if len(pushes) >= 2:
                h, cat = pushes[0], pushes[1]
                table.setdefault(f"{h:#010x}", {"name": vtables[vt], "category": f"{cat:#010x}", "vtable": f"{vt:#x}", "ctor": f"{fs:#x}"})
    json.dump(dict(sorted(table.items())), open(sys.argv[2], "w", encoding="utf-8"), indent=1)
    cats = {}
    for v in table.values():
        cats.setdefault(v["category"], []).append(v["name"])
    print(f"messages resolved: {len(table)} of {len(types)} reflected *Message types; getType fns {len(gettype)}, vtables {len(vtables)}")
    for c, names in sorted(cats.items(), key=lambda x: -len(x[1]))[:12]:
        print(f"  category {c}: {len(names)} e.g. {names[:3]}")


if __name__ == "__main__":
    main()

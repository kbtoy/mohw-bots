"""Resolve FB2 message plumbing for a message type name (from re/sdk/types.json):
getType() function, vtable, constructor(s) + type hash, then senders (ctor callers) and handlers
(code comparing against the hash).

Usage: python -I msginfo.py <pe> <MessageName> [<MessageName> ...]
Pattern facts (lanmohw.exe): getType = `mov eax, <TypeInfo>; ret` (B8 imm32 C3), referenced from the
vtable at slot 0; ctor = `push <hash>; push <category>; ... call fb::Message::Message (0x584680)` then
`mov dword ptr [esi], <vtable>`; handlers `cmp reg, <hash>` on `msg+8`.
"""
import json
import os
import re
import struct
import sys

import pefile

MESSAGE_CTOR = 0x584680


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
    types = {t["name"]: t for t in json.load(open(os.path.join(root, "re", "sdk", "types.json"), encoding="utf-8")) if t["name"]}
    code = [(base + s.VirtualAddress, img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize])
            for s in pe.sections if s.Characteristics & 0x20000000]

    def find_all(blob):
        hits = []
        for start, data in code:
            hits += [start + m.start() for m in re.finditer(re.escape(blob), data)]
        return hits

    def rel_callers(target):
        out = []
        for start, data in code:
            for m in re.finditer(rb"\xe8", data):
                o = m.start()
                if o + 5 <= len(data) and start + o + 5 + struct.unpack_from("<i", data, o + 1)[0] == target:
                    out.append(start + o)
        return out

    for name in sys.argv[2:]:
        t = types.get(name)
        if not t:
            print(f"### {name}: not in SDK")
            continue
        ti = t["typeinfo"]
        print(f"### {name}  TypeInfo {ti:#x}  size {t['size']:#x}")
        gettype = [a for a in find_all(b"\xb8" + struct.pack("<I", ti) + b"\xc3")]
        for g in gettype:
            vt_refs = [base + m.start() for m in re.finditer(re.escape(struct.pack("<I", g)), img)]
            print(f"  getType {g:#x}; referenced at {[hex(v) for v in vt_refs]}")
            for slot in vt_refs:
                vtable = slot  # getType is slot 0 (verified on AISpawnBotMessage, vtable 0x27b690c)
                writers = find_all(struct.pack("<I", vtable))
                for w in writers:
                    ctor = func_start(img, base, w)
                    window = img[ctor - base:w - base]
                    hashes = [struct.unpack("<I", window[m.start() + 1:m.start() + 5])[0]
                              for m in re.finditer(rb"\x68", window)][:2]
                    print(f"  vtable {vtable:#x} written at {w:#x} in ctor {ctor:#x}; pushes {[hex(h) for h in hashes]}")
                    if hashes:
                        h = hashes[0]
                        uses = [u for u in find_all(struct.pack("<I", h)) if not (ctor <= u < w)]
                        print(f"    hash {h:#010x} used at: " + ", ".join(f"{u:#x} (func {func_start(img, base, u):#x})" for u in uses[:12]))
                    callers = rel_callers(ctor)
                    print(f"    ctor called from: " + ", ".join(f"{c:#x} (func {func_start(img, base, c):#x})" for c in callers[:12]))


if __name__ == "__main__":
    main()

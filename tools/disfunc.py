"""Annotated disassembly of whole functions in a 32-bit PE (lanmohw.exe by default).

Usage: python -I disfunc.py <pe> <0xVA> [<0xVA> ...] [--max=N] [--types=re/sdk/types.json] [--labels=re/labels.json]

Disassembles linearly from each address until the function's int3/nop padding after a
terminating instruction (good enough for MSVC output). Annotates:
  - ASCII / UTF-16 strings, imports (call [IAT]), TypeInfo objects and data blocks (via types.json)
  - addresses named in labels.json ({"0x584680": "fb::Message::Message", ...})
"""
import json
import os
import re
import struct
import sys

import capstone
import pefile


def load_annotations(pe, base, types_path, labels_path):
    names = {}
    if types_path and os.path.exists(types_path):
        for t in json.load(open(types_path, encoding="utf-8")):
            n = t.get("display_name") or t["name"] or f"type_{t['typeinfo']:#x}"
            names[t["typeinfo"]] = f"TypeInfo<{n}>"
            names[t["typeinfo"] + 4] = f"TypeInfo<{n}>(+4)"
            names[t["data"]] = f"TypeData<{n}>"
            if t.get("create"):
                names[t["create"]] = f"{n}::create"
    for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for i in imp.imports:
            if i.name:
                names[i.address] = f"{imp.dll.decode()}!{i.name.decode()}"
    if labels_path and os.path.exists(labels_path):
        for k, v in json.load(open(labels_path, encoding="utf-8")).items():
            names[int(k, 16)] = v
    return names


class Disasm:
    def __init__(self, path, types_path, labels_path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.names = load_annotations(self.pe, self.base, types_path, labels_path)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False

    def valid(self, va):
        return self.base <= va < self.base + len(self.img)

    def describe(self, va):
        if va in self.names:
            return self.names[va]
        if not self.valid(va):
            return None
        raw = self.img[va - self.base:va - self.base + 120]
        s = raw.split(b"\0")[0]
        if len(s) >= 3 and all(32 <= c < 127 for c in s):
            return repr(s.decode())
        if len(raw) > 6 and raw[1] == 0 and raw[3] == 0 and 32 <= raw[0] < 127:
            w = raw.decode("utf-16le", "replace").split("\0")[0]
            if len(w) >= 3:
                return "L" + repr(w)
        return None

    def function(self, start, max_insns=600):
        out = []
        code = self.img[start - self.base:start - self.base + max_insns * 15]
        prev_terminal = False
        for ins in self.md.disasm(code, start):
            if prev_terminal and ins.mnemonic in ("int3", "nop") and not out[-1][1].startswith("jmp dword ptr [e"):
                break
            notes = []
            for imm in re.findall(r"0x[0-9a-f]{5,8}", ins.op_str):
                d = self.describe(int(imm, 16))
                if d:
                    notes.append(d)
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                tgt = int(ins.op_str, 16)
                if tgt not in self.names and not notes:
                    notes.append(f"sub_{tgt:X}")
            text = f"{ins.mnemonic:7s} {ins.op_str}"
            out.append((ins.address, text, notes))
            prev_terminal = ins.mnemonic in ("ret", "jmp") or (ins.mnemonic == "int3" and prev_terminal)
            if len(out) >= max_insns:
                break
        return out

    def print_function(self, start, max_insns=600):
        label = self.names.get(start, f"sub_{start:X}")
        print(f"\n===== {label} @ {start:#x}")
        for addr, text, notes in self.function(start, max_insns):
            print(f"  {addr:08x}: {text}" + (f"   ; {' | '.join(notes)}" if notes else ""))


def main():
    sys.stdout.reconfigure(errors="replace")  # annotated strings may not fit the console code page
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    d = Disasm(args[0], opts.get("types", os.path.join(root, "re", "sdk", "types.json")),
               opts.get("labels", os.path.join(root, "re", "labels.json")))
    for a in args[1:]:
        d.print_function(int(a, 16), int(opts.get("max", 600)))


if __name__ == "__main__":
    main()

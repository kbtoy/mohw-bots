"""List call sites of given imports in a 32-bit PE and disassemble around each.

Usage: python -I import_calls.py <pe> <ImportName> [<ImportName> ...] [--before=N] [--after=N]
Finds `call/jmp [IAT]` and `mov reg, [IAT]` uses of the import's IAT slot.
"""
import re
import struct
import sys

import capstone
import pefile


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    before, after = int(opts.get("before", 40)), int(opts.get("after", 40))
    pe = pefile.PE(args[0])
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    iat = {}
    for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for i in imp.imports:
            if i.name:
                iat[i.name.decode()] = (imp.dll.decode(), i.address)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for name in args[1:]:
        if name not in iat:
            print(f"### {name}: not imported")
            continue
        dll, slot = iat[name]
        print(f"### {dll}!{name} IAT slot {hex(slot)}")
        packed = struct.pack("<I", slot)
        for sec in pe.sections:
            if not sec.Characteristics & 0x20000000:
                continue
            lo = sec.VirtualAddress
            for m in re.finditer(re.escape(packed), img[lo:lo + sec.Misc_VirtualSize]):
                ref = base + lo + m.start()
                start = ref - before
                code = img[start - base:ref - base + after]
                for skew in range(16):
                    insns = list(md.disasm(code[skew:], start + skew))
                    if any(i.address <= ref < i.address + i.size for i in insns):
                        break
                print(f"  -- use at {hex(ref)}")
                for i in insns:
                    mark = "=>" if i.address <= ref < i.address + i.size else "  "
                    extra = ""
                    for imm in re.findall(r"0x[0-9a-f]{8}", i.op_str):
                        v = int(imm, 16)
                        if base <= v < base + len(img):
                            s = img[v - base:v - base + 60].split(b"\0")[0]
                            if len(s) >= 3 and all(32 <= c < 127 for c in s):
                                extra = f"   ; {s.decode()!r}"
                    print(f"     {mark} {i.address:08x}: {i.mnemonic:8s} {i.op_str}{extra}")


if __name__ == "__main__":
    main()

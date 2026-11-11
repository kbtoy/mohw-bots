"""Find uses of entries in a runtime-resolved API pointer table.

Usage: python -I table_uses.py <pe> <table_global_va> <name=offset> [<name=offset> ...] [--ctx=N]
A "use" is `[reg + offset]` where reg was loaded from [table_global] within the previous few
instructions. Linear-sweep disassembly, so treat results as leads, not proof of absence.
"""
import re
import sys

import capstone
import pefile


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    ctx = int(opts.get("ctx", 14))
    pe = pefile.PE(args[0], fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    table = int(args[1], 16)
    wanted = {int(v, 16): k for k, v in (a.split("=") for a in args[2:])}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    load_re = re.compile(r"^(e[a-d]x|e[sd]i|ebp), dword ptr \[0x%x\]$" % table)
    for sec in pe.sections:
        if not sec.Characteristics & 0x20000000:
            continue
        lo = sec.VirtualAddress
        insns = list(md.disasm(img[lo:lo + sec.Misc_VirtualSize], base + lo))
        for idx, ins in enumerate(insns):
            m = re.search(r"\[(e[a-d]x|e[sd]i|ebp) \+ (0x[0-9a-f]+)\]", ins.op_str)
            if not m or int(m.group(2), 16) not in wanted:
                continue
            reg = m.group(1)
            loaded = any(p.mnemonic == "mov" and load_re.match(p.op_str) and p.op_str.startswith(reg)
                         for p in insns[max(0, idx - int(opts.get("window", 8))):idx])
            if not loaded or (ins.mnemonic == "mov" and ins.op_str.startswith("dword ptr [esi + ") and ins.op_str.endswith("], eax")):
                continue
            print(f"### {wanted[int(m.group(2), 16)]} used at {ins.address:08x}")
            for p in insns[max(0, idx - ctx):idx + 4]:
                extra = ""
                for imm in re.findall(r"0x[0-9a-f]{6,8}", p.op_str):
                    v = int(imm, 16)
                    if base <= v < base + len(img):
                        s = img[v - base:v - base + 80].split(b"\0")[0]
                        if len(s) >= 4 and all(32 <= c < 127 for c in s):
                            extra = f"   ; {s.decode()!r}"
                        elif len(s) >= 8 and s[1::2] == b"\0" * (len(s) // 2):
                            extra = f"   ; L{img[v - base:v - base + 160].decode('utf-16le', 'replace').split(chr(0))[0]!r}"
                print(f"   {'=>' if p.address == ins.address else '  '} {p.address:08x}: {p.mnemonic:8s} {p.op_str}{extra}")


if __name__ == "__main__":
    main()

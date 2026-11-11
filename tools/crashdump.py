"""Summarise a lanmohw.exe crash dump: exception, faulting thread registers, and the return addresses on its stack
that point into the game's code (with labels from re/labels.json).

Usage: python -I crashdump.py <file.dmp> [--words=N]
"""
import json
import os
import struct
import sys

from minidump.minidumpfile import MinidumpFile

GAME_TEXT = (0x401000, 0x2C64000)  # lanmohw.exe .text .. .666


def main():
    path = sys.argv[1]
    words = int(next((a.split("=")[1] for a in sys.argv if a.startswith("--words=")), 96))
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    labels = {int(k, 16): v for k, v in json.load(open(os.path.join(root, "re", "labels.json"), encoding="utf-8")).items()}

    mf = MinidumpFile.parse(path)
    reader = mf.get_reader()
    exc = mf.exception.exception_records[0]
    rec = exc.ExceptionRecord
    print(f"exception code {rec.ExceptionCode} at {rec.ExceptionAddress:#x} thread {exc.ThreadId}")
    print(f"  info: {[hex(x) for x in rec.ExceptionInformation[:rec.NumberParameters]]}")

    # The exception stream's ThreadContext is a location in the dump file, not process memory.
    with open(path, "rb") as fh:
        fh.seek(exc.ThreadContext.Rva)
        ctx_raw = fh.read(exc.ThreadContext.DataSize)
    # x86 CONTEXT: ContextFlags, Dr0-3,6,7, FloatSave(112), Gs Fs Es Ds Edi Esi Ebx Edx Ecx Eax Ebp Eip SegCs EFlags Esp SegSs
    off = 4 + 6 * 4 + 112
    regs = struct.unpack_from("<16I", ctx_raw, off)
    names = ["Gs", "Fs", "Es", "Ds", "Edi", "Esi", "Ebx", "Edx", "Ecx", "Eax", "Ebp", "Eip", "SegCs", "EFlags", "Esp", "SegSs"]
    r = dict(zip(names, regs))
    print("  " + " ".join(f"{n}={r[n]:08x}" for n in ("Eip", "Esp", "Ebp", "Eax", "Ecx", "Edx", "Ebx", "Esi", "Edi")))

    esp = r["Esp"]
    try:
        stack = reader.read(esp, words * 4)
    except Exception as ex:  # partial stack
        print("  stack read failed:", ex)
        return
    print(f"  stack from {esp:#x}: return addresses into lanmohw.exe .text")
    for i in range(words):
        v = struct.unpack_from("<I", stack, i * 4)[0]
        if GAME_TEXT[0] <= v < GAME_TEXT[1]:
            print(f"    [esp+{i * 4:#05x}] {v:#010x}  {labels.get(v, '')}")

    print("  modules near fault:")
    for m in mf.modules.modules:
        if m.baseaddress <= r["Eip"] < m.baseaddress + m.size:
            print(f"    EIP inside {m.name}")


if __name__ == "__main__":
    main()

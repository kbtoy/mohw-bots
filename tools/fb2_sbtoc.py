"""Frostbite 2 TOC / superbundle reader (BF3-era "DbObject" binary format), read-only.

TOC files start with 00 D1 CE 00|01; the payload begins at 0x22C and is XORed with a 257-byte key
stored at 0x128 (each key byte ^ 0x7B) — per Frankelstner's BF3 tools.

DbObject entry: type byte; if not (type & 0x80) a NUL-terminated name follows; payload by (type & 0x1F):
  0x01 list  (LEB128 size, entries until 0x00)     0x02 object (LEB128 size, entries until 0x00)
  0x06 bool  (1 byte)   0x07 string (LEB128 len incl. NUL)   0x08 int32   0x09 int64
  0x0F guid  (16)       0x10 sha1 (20)                       0x13 blob (LEB128 len)

Usage:
  python -I fb2_sbtoc.py toc <file.toc>                 # summary of a TOC
  python -I fb2_sbtoc.py bundles <file.toc> [--ebx]     # list bundles (and their EBX names from the .sb)
"""
import io
import struct
import sys


def read_leb128(f):
    result = shift = 0
    while True:
        b = f.read(1)[0]
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result
        shift += 7


def read_cstr(f):
    out = bytearray()
    while True:
        c = f.read(1)
        if not c or c == b"\0":
            return out.decode("utf-8", "replace")
        out += c


def read_entry(f):
    """Return (name, value) or None at end marker."""
    t = f.read(1)
    if not t:
        return None
    t = t[0]
    if t == 0:
        return None
    name = None if t & 0x80 else read_cstr(f)
    kind = t & 0x1F
    if kind in (0x01, 0x02):
        size = read_leb128(f)
        end = f.tell() + size
        items = []
        while f.tell() < end:
            e = read_entry(f)
            if e is None:
                break
            items.append(e)
        f.seek(end)
        value = [v for _, v in items] if kind == 0x01 else {k: v for k, v in items}
    elif kind == 0x06:
        value = bool(f.read(1)[0])
    elif kind == 0x07:
        n = read_leb128(f)
        value = f.read(n).rstrip(b"\0").decode("utf-8", "replace")
    elif kind == 0x08:
        value = struct.unpack("<i", f.read(4))[0]
    elif kind == 0x09:
        value = struct.unpack("<q", f.read(8))[0]
    elif kind == 0x0F:
        value = f.read(16).hex()
    elif kind == 0x10:
        value = f.read(20).hex()
    elif kind == 0x13:
        value = f.read(read_leb128(f))
    else:
        raise ValueError(f"unknown DbObject type {t:#x} at {f.tell() - 1:#x}")
    return name, value


def read_dbobject(data, offset=0):
    f = io.BytesIO(data)
    f.seek(offset)
    e = read_entry(f)
    return e[1] if e else None


def load_toc(path):
    raw = open(path, "rb").read()
    if raw[:4] in (b"\x00\xD1\xCE\x00", b"\x00\xD1\xCE\x01"):
        # Both variants are XOR-obfuscated in MOHW (verified on MP_03.toc).
        key = [b ^ 0x7B for b in raw[0x128:0x128 + 257]]
        body = bytes(key[i % 257] ^ c for i, c in enumerate(raw[0x22C:]))
    else:
        body = raw
    return read_dbobject(body)


def bundle_manifests(toc_path):
    """Yield (bundle_id, manifest_dict) for a cas-style superbundle."""
    toc = load_toc(toc_path)
    sb = open(toc_path[:-4] + ".sb", "rb").read()
    for b in toc.get("bundles", []):
        man = read_dbobject(sb, b["offset"]) if toc.get("cas") else None
        yield b, man


def is_cas(toc_path):
    return bool(load_toc(toc_path).get("cas"))


def noncas_ebx(sb_path, offset):
    """Yield (name, data) for each EBX of the bundle at `offset` in a non-cas superbundle (the expansion's maps,
    Update/Xpack1): BF3's binary bundle. u32 BE metaSize, then 8 u32 BE (magic 0x970D1C13, entries, ebx, res, chunks,
    stringOffset, chunkMetaOffset, chunkMetaSize; offsets from after metaSize), a sha1 per entry, then per EBX
    {nameOffset, storedSize, originalSize}. The payloads follow the meta in entry order (EBX first), each in the cas
    block format and 16-byte aligned from the payload start (verified on MP_21_sc: all 834 EBX decode)."""
    import fb2_cas  # noqa: E402 (fb2_cas imports nothing from here)
    with open(sb_path, "rb") as f:
        f.seek(offset)
        meta_size, = struct.unpack(">I", f.read(4))
        meta_start = offset + 4
        magic, total, n_ebx, _res, _chunks, string_offset, _cmo, _cms = struct.unpack(">8I", f.read(32))
        if magic != 0x970D1C13:
            raise ValueError(f"not a binary bundle at {offset:#x} (magic {magic:#x})")
        f.seek(20 * total, 1)
        entries = [struct.unpack(">3I", f.read(12)) for _ in range(n_ebx)]
        names = []
        for name_offset, _, _ in entries:
            f.seek(meta_start + string_offset + name_offset)
            raw = b""
            while True:
                c = f.read(1)
                if c in (b"", b"\x00"):
                    break
                raw += c
            names.append(raw.decode("utf-8", "replace"))
        base = pos = meta_start + meta_size
        for name, (_, size, original) in zip(names, entries):
            f.seek(pos)
            data = fb2_cas.decompress(f.read(size))
            pos = base + ((pos + size - base + 15) & ~15)
            if len(data) == original:
                yield name, data


def short(v, n=6):
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k}: {short(x, n)}" for k, x in list(v.items())[:n]) + (" ..." if len(v) > n else "") + "}"
    if isinstance(v, list):
        return f"[{len(v)} items: " + ", ".join(short(x, n) for x in v[:2]) + (" ..." if len(v) > 2 else "") + "]"
    if isinstance(v, (bytes, bytearray)):
        return f"<blob {len(v)}>"
    return repr(v)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "toc":
        toc = load_toc(path)
        for k, v in toc.items():
            print(f"{k}: {short(v)}")
    elif cmd == "bundles":
        show_ebx = "--ebx" in sys.argv
        for b, man in bundle_manifests(path):
            line = f"{b.get('id')}  (sb offset {b.get('offset'):#x}, size {b.get('size'):#x})"
            if man:
                line += f"  ebx={len(man.get('ebx', []))} res={len(man.get('res', []))} chunks={len(man.get('chunks', []))}"
            print(line)
            if show_ebx and man:
                for e in man.get("ebx", []):
                    print(f"    ebx {e.get('name')}")


if __name__ == "__main__":
    main()

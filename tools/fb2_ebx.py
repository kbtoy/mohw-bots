"""Frostbite 2 EBX (partition) reader, read-only. Layout per Frankelstner's BF3 ebx tools, little endian.

Header (after magic 0x0FB2D1CE), 11×u32: absStringOffset, lenStringToEOF, numGUID, null, numInstanceRepeater,
  numComplex, numField, lenName, lenString, numArrayRepeater, lenPayload   (verified on MOHW: sections sum
  exactly to absStringOffset)
then fileGUID[16], primaryInstanceGUID[16], external GUID pairs, keyword names, field descriptors (16 B),
complex descriptors (16 B), instance repeaters (3×u32: null, repetitions, complexIndex), pad to 16,
array repeaters (3×u32: offset, repetitions, complexIndex). Every instance is prefixed by its GUID.
Payload at absStringOffset + lenString; array payload at + lenPayload.

Field descriptor type: basic type code = (type >> 4) & 0x1F (same codes as TypeInfo flags).

Usage: python -I fb2_ebx.py <file.ebx> [--max-depth=N]   # prints instances as an indented tree
"""
import struct
import sys

CODES = ["Void", "DbObject", "ValueType", "Class", "Array", "FixedArray", "String", "CString", "Enum",
         "FileRef", "Boolean", "Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64",
         "Float32", "Float64", "Guid", "SHA1", "ResourceRef"]
PRIM = {"Boolean": ("<?", 1), "Int8": ("<b", 1), "Uint8": ("<B", 1), "Int16": ("<h", 2), "Uint16": ("<H", 2),
        "Int32": ("<i", 4), "Uint32": ("<I", 4), "Int64": ("<q", 8), "Uint64": ("<Q", 8), "Float32": ("<f", 4),
        "Float64": ("<d", 8), "Enum": ("<i", 4)}


def keyword_hash(name: bytes) -> int:
    h = 5381
    for c in name:
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h


class Ref:
    """Pointer field: internal instance index, external (fileGUID, instanceGUID), or null."""

    def __init__(self, kind, target):
        self.kind, self.target = kind, target

    def __repr__(self):
        if self.kind == "null":
            return "null"
        if self.kind == "internal":
            return f"#{self.target}"
        return f"ext:{self.target[0].hex()}/{self.target[1].hex()}"


class Ebx:
    def __init__(self, data: bytes):
        self.d = data
        assert struct.unpack_from("<I", data, 0)[0] == 0x0FB2D1CE, "not an FB2 EBX"
        (self.absStringOffset, self.lenStringToEOF, self.numGUID, _null, self.numInstanceRepeater,
         self.numComplex, self.numField, self.lenName, self.lenString, self.numArrayRepeater,
         self.lenPayload) = struct.unpack_from("<11I", data, 4)
        p = 4 + 44
        self.fileGUID = data[p:p + 16]
        self.primaryInstanceGUID = data[p + 16:p + 32]
        p += 32
        self.externalGUIDs = []
        for _ in range(self.numGUID):
            self.externalGUIDs.append((data[p:p + 16], data[p + 16:p + 32]))
            p += 32
        names = data[p:p + self.lenName].split(b"\0")
        self.names = {keyword_hash(n): n.decode("utf-8", "replace") for n in names if n}
        p += self.lenName
        self.fields = []
        for _ in range(self.numField):
            h, typ, ref, off, off2 = struct.unpack_from("<IHHii", data, p)
            self.fields.append({"name": self.names.get(h, f"{h:#x}"), "type": typ, "code": CODES[(typ >> 4) & 0x1F]
                                if ((typ >> 4) & 0x1F) < len(CODES) else f"code{(typ >> 4) & 0x1F}",
                                "ref": ref, "offset": off})
            p += 16
        self.complexes = []
        for _ in range(self.numComplex):
            h, start, nfield, align, typ, size, size2 = struct.unpack_from("<IIBBHHH", data, p)
            self.complexes.append({"name": self.names.get(h, f"{h:#x}"), "start": start, "count": nfield,
                                   "align": align or 1, "type": typ, "size": size})
            p += 16
        self.instanceRepeaters = []
        for _ in range(self.numInstanceRepeater):
            _n, reps, cidx = struct.unpack_from("<3I", data, p)
            self.instanceRepeaters.append((cidx, reps))
            p += 12
        p = (p + 15) & ~15
        self.arrayRepeaters = []
        for _ in range(self.numArrayRepeater):
            self.arrayRepeaters.append(struct.unpack_from("<3I", data, p))  # offset, repetitions, complexIndex
            p += 12
        self.payloadStart = self.absStringOffset + self.lenString
        self.arrayStart = self.payloadStart + self.lenPayload
        self.instances = []  # (guid_or_index, complexIndex, offset)
        pos = self.payloadStart
        for cidx, reps in self.instanceRepeaters:
            c = self.complexes[cidx]
            for _ in range(reps):
                # MOHW packs instances back to back (no alignment padding) — verified: with padding MP_03
                # overshoots lenPayload by 0x20 and misses the primary GUID; without, the primary GUID lands
                # where expected in every level tested (lenPayload itself includes trailing padding).
                guid = data[pos:pos + 16]  # every instance is prefixed by its GUID
                pos += 16
                self.instances.append((guid, cidx, pos))
                pos += c["size"]
        self.payloadEnd = pos

    def cstr(self, off):
        if off == -1:
            return None
        s = self.absStringOffset + off
        return self.d[s:self.d.index(b"\0", s)].decode("utf-8", "replace")

    def read_value(self, field, pos):
        code = field["code"]
        if code in PRIM:
            fmt, _ = PRIM[code]
            return struct.unpack_from(fmt, self.d, pos)[0]
        if code in ("CString", "FileRef"):
            return self.cstr(struct.unpack_from("<i", self.d, pos)[0])
        if code == "Guid":
            return self.d[pos:pos + 16].hex()
        if code == "SHA1":
            return self.d[pos:pos + 20].hex()
        if code == "ResourceRef":
            return struct.unpack_from("<Q", self.d, pos)[0]
        if code == "Class":
            v = struct.unpack_from("<I", self.d, pos)[0]
            if v >> 31:
                return Ref("external", self.externalGUIDs[v & 0x7FFFFFFF])
            return Ref("internal", v - 1) if v else Ref("null", None)
        if code in ("ValueType", "Void", "DbObject"):
            return self.read_complex(field["ref"], pos)
        if code == "Array":
            off, reps, cidx = self.arrayRepeaters[struct.unpack_from("<I", self.d, pos)[0]]
            member = self.fields[self.complexes[field["ref"]]["start"]]
            step = self.elem_size(member)
            base = self.arrayStart + off
            return [self.read_value(member, base + k * step) for k in range(reps)]
        return f"<{code} {field['type']:#x}>"

    def elem_size(self, field):
        code = field["code"]
        if code in PRIM:
            return PRIM[code][1]
        if code in ("ValueType", "Void", "DbObject"):
            return self.complexes[field["ref"]]["size"]
        return {"Guid": 16, "SHA1": 20, "ResourceRef": 8}.get(code, 4)

    def read_complex(self, cidx, pos):
        c = self.complexes[cidx]
        out = {"$type": c["name"]}
        for f in self.fields[c["start"]:c["start"] + c["count"]]:
            if f["code"] == "Void" and f["name"] == "$":
                base = self.read_complex(f["ref"], pos + f["offset"])
                base.pop("$type", None)
                out.update(base)  # flatten inherited fields
                continue
            out[f["name"]] = self.read_value(f, pos + f["offset"])
        return out

    def instance(self, i):
        guid, cidx, pos = self.instances[i]
        return self.read_complex(cidx, pos)

    def type_counts(self):
        from collections import Counter
        return Counter(self.complexes[c]["name"] for _, c, _ in self.instances)


def dump(v, depth=0, max_depth=4):
    pad = "  " * depth
    if isinstance(v, dict):
        if depth >= max_depth:
            return f"{{{v.get('$type', '')}...}}"
        lines = [f"{v.get('$type', '')} {{"]
        for k, x in v.items():
            if k != "$type":
                lines.append(f"{pad}  {k}: {dump(x, depth + 1, max_depth)}")
        lines.append(f"{pad}}}")
        return "\n".join(lines)
    if isinstance(v, list):
        if len(v) > 8:
            return f"[{len(v)} items: " + ", ".join(dump(x, depth + 1, max_depth) for x in v[:3]) + ", ...]"
        return "[" + ", ".join(dump(x, depth + 1, max_depth) for x in v) + "]"
    return repr(v)


if __name__ == "__main__":
    e = Ebx(open(sys.argv[1], "rb").read())
    md = int(next((a.split("=")[1] for a in sys.argv if a.startswith("--max-depth=")), 3))
    print(f"instances: {len(e.instances)}  types: {dict(e.type_counts().most_common(12))}")
    print(dump(e.instance(0), 0, md))

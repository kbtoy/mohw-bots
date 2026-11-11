"""Static Frostbite 2 (32-bit) TypeInfo dumper.

Works on a decrypted exe without running it. Frostbite 2 keeps the const reflection data
(MemberInfoData / TypeInfoData / ClassInfoData / FieldInfoData) in the image, but the runtime
TypeInfo objects are zero until static constructors run. Those constructors look like

    push <TypeInfoData*>      ; 68 imm32
    mov  ecx, <TypeInfo*>     ; B9 imm32
    call <XxxInfo::XxxInfo>   ; E8 rel32

so scanning for that pattern recovers the TypeInfo* -> TypeInfoData* map statically.

Layouts follow txt231/FBTools (Sdk/Fb2), verified against MOHW's lanmohw.exe:
    MemberInfoData  { const char* name; u16 flags; }                       (pack 2, 6 bytes)
    TypeInfoData    : MemberInfoData { u16 size; ModuleInfo* module; u8 align; u8 fieldCount; u8 pad[2]; }  (0x10)
    ClassInfoData   : TypeInfoData { ClassInfo* super; void* create; FieldInfoData* fields; }
    ValueTypeInfoData : TypeInfoData { void* defaultValue; FieldInfoData* fields; }
    EnumTypeInfoData  : TypeInfoData { FieldInfoData* fields; }
    ArrayTypeInfoData : TypeInfoData { TypeInfo* elementType; }
    FieldInfoData   : MemberInfoData { u16 offset; TypeInfo* type /* or enum value */; }   (0x0C)
    flags: memberType = f & 3, category = (f >> 2) & 3, typeCode = (f >> 4) & 0x1F

Usage: python -I fb2_typeinfo_dump.py <exe> <out_dir>
Writes <out_dir>/types.json, <out_dir>/sdk.h, <out_dir>/summary.txt
"""
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict

import pefile

TYPE_CODES = ["Void", "DbObject", "ValueType", "Class", "Array", "FixedArray", "String", "CString", "Enum",
              "FileRef", "Boolean", "Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Int64", "Uint64",
              "Float32", "Float64", "Guid", "SHA1", "ResourceRef"]
CPP_PRIMITIVES = {"Boolean": "bool", "Int8": "int8_t", "Uint8": "uint8_t", "Int16": "int16_t", "Uint16": "uint16_t",
                  "Int32": "int32_t", "Uint32": "uint32_t", "Int64": "int64_t", "Uint64": "uint64_t",
                  "Float32": "float", "Float64": "double", "CString": "const char*", "String": "fb::String",
                  "FileRef": "fb::FileRef", "Guid": "fb::Guid", "SHA1": "fb::SHA1", "ResourceRef": "fb::ResourceRef"}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.sections = {s.Name.rstrip(b"\0").decode(): s for s in self.pe.sections}

    def valid(self, va):
        return self.base <= va < self.base + len(self.img)

    def u8(self, va):
        return self.img[va - self.base]

    def u16(self, va):
        return struct.unpack_from("<H", self.img, va - self.base)[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.img, va - self.base)[0]

    def cstr(self, va, limit=256):
        if not self.valid(va):
            return None
        raw = self.img[va - self.base:va - self.base + limit].split(b"\0")[0]
        if all(32 <= c < 127 for c in raw):
            return raw.decode()
        return None

    def section_of(self, va):
        for n, s in self.sections.items():
            if s.VirtualAddress <= va - self.base < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                return n
        return None

    def section_bytes(self, name):
        s = self.sections[name]
        return self.base + s.VirtualAddress, self.img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]


def find_registrations(im):
    """Return {typeinfo_obj_va: (data_va, ctor_va)} from static-constructor code."""
    pattern = re.compile(rb"\x68(.{4})\xB9(.{4})\xE8(.{4})", re.S)
    regs = {}
    for name in ("ctr", ".text"):
        if name not in im.sections:
            continue
        start, data = im.section_bytes(name)
        for m in pattern.finditer(data):
            data_va, obj_va = struct.unpack("<II", m.group(1) + m.group(2))
            ctor = start + m.end() + struct.unpack("<i", m.group(3))[0]
            if not (im.valid(data_va) and im.valid(obj_va)):
                continue
            name_ptr = im.u32(data_va)
            flags = im.u16(data_va + 4)
            if flags & 3 != 1:  # memberType must be TypeInfo
                continue
            type_name = im.cstr(name_ptr) if im.valid(name_ptr) else None
            if type_name is None:
                continue
            regs.setdefault(obj_va, (data_va, ctor))
    return regs


def read_fields(im, ptr, count):
    fields = []
    for i in range(count):
        f = ptr + i * 0xC
        if not im.valid(f + 0xC):
            break
        fields.append({
            "name": im.cstr(im.u32(f)) or "",
            "flags": im.u16(f + 4),
            "offset": im.u16(f + 6),
            "raw": im.u32(f + 8),
        })
    return fields


def dump(path, out_dir):
    im = Image(path)
    regs = find_registrations(im)
    obj_to_data = {obj: data for obj, (data, _) in regs.items()}
    types = {}
    for obj, (data, ctor) in regs.items():
        flags = im.u16(data + 4)
        code = (flags >> 4) & 0x1F
        t = {
            "name": im.cstr(im.u32(data)) or "",
            "typeinfo": obj,
            "data": data,
            "ctor": ctor,
            "flags": flags,
            "category": (flags >> 2) & 3,
            "code": TYPE_CODES[code] if code < len(TYPE_CODES) else f"code{code}",
            "size": im.u16(data + 6),
            "module": im.u32(data + 8),
            "alignment": im.u8(data + 0xC),
            "field_count": im.u8(data + 0xD),
        }
        if t["code"] == "Class":
            t["super"] = im.u32(data + 0x10)
            t["create"] = im.u32(data + 0x14)
            t["fields"] = read_fields(im, im.u32(data + 0x18), t["field_count"]) if t["field_count"] else []
        elif t["code"] == "ValueType":
            t["default_value"] = im.u32(data + 0x10)
            t["fields"] = read_fields(im, im.u32(data + 0x14), t["field_count"]) if t["field_count"] else []
        elif t["code"] == "Enum":
            t["fields"] = read_fields(im, im.u32(data + 0x10), t["field_count"]) if t["field_count"] else []
        elif t["code"] in ("Array", "FixedArray"):
            t["element"] = im.u32(data + 0x10)
        types[obj] = t

    def type_name(obj):
        # Some TypeInfos (mostly arrays of primitives) are embedded at +4 in an object whose
        # constructor (`mov ecx, obj; call`) is followed by a vtable store at obj+0, so fields
        # reference obj+4 while the registration names obj.
        t = types.get(obj) or types.get(obj - 4)
        if t is None:
            return f"/*unresolved {obj:#x}*/void"
        if t["code"] in ("Array", "FixedArray") and not t["name"]:
            return f"Array<{type_name(t['element'])}>"
        return t["name"] or f"type_{obj:#x}"

    # resolve names for output
    for t in types.values():
        if "super" in t:
            # Root runtime classes point m_super at themselves (directly or via the +4 alias).
            is_self = t["super"] in (t["typeinfo"], t["typeinfo"] + 4)
            t["super_name"] = type_name(t["super"]) if t["super"] and not is_self else None
        if t["code"] in ("Array", "FixedArray"):
            t["element_name"] = type_name(t["element"])
            if not t["name"]:
                t["display_name"] = type_name(t["typeinfo"])
        for f in t.get("fields", []):
            if t["code"] == "Enum":
                f["value"] = struct.unpack("<i", struct.pack("<I", f["raw"]))[0]
            else:
                f["type"] = type_name(f["raw"])

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "types.json"), "w", encoding="utf-8") as fh:
        json.dump(sorted(types.values(), key=lambda t: (t["code"], t["name"])), fh, indent=1,
                  default=lambda o: o)

    write_header(types, type_name, os.path.join(out_dir, "sdk.h"))

    counts = Counter(t["code"] for t in types.values())
    unresolved = Counter()
    for t in types.values():
        for f in t.get("fields", []):
            if "type" in f and f["type"].startswith("/*unresolved"):
                unresolved[f["raw"]] += 1
    with open(os.path.join(out_dir, "summary.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"source: {path}\nregistrations found: {len(regs)}\n")
        for k, v in counts.most_common():
            fh.write(f"  {k:12s} {v}\n")
        fh.write(f"field types unresolved: {sum(unresolved.values())} refs to {len(unresolved)} TypeInfo objects\n")
        for va, n in unresolved.most_common(20):
            fh.write(f"  {va:#010x} x{n} section={im.section_of(va)}\n")
    return types, counts, unresolved


def cpp_field_type(f, types_by_name):
    name = f["type"]
    t = types_by_name.get(name)
    if name in CPP_PRIMITIVES:
        return CPP_PRIMITIVES[name]
    if t is None:
        if name.startswith("Array<"):
            inner = name[6:-1]
            inner_t = types_by_name.get(inner)
            inner_cpp = CPP_PRIMITIVES.get(inner, inner + ("*" if inner_t and inner_t["code"] == "Class" else ""))
            return f"fb::Array<{inner_cpp}>"
        return name
    if t["code"] == "Class":
        return name + "*"
    return name


def write_header(types, type_name, path):
    by_name = {}
    for t in types.values():
        by_name.setdefault(t.get("display_name") or t["name"], t)
    lines = ["// Generated by tools/fb2_typeinfo_dump.py from lanmohw.exe (Medal of Honor: Warfighter, 32-bit FB2).",
             "// Offsets are absolute within the object. Pointers to reflected classes are raw pointers (FB2 32-bit).",
             "#pragma once", "#include <cstdint>", ""]
    for t in sorted((t for t in types.values() if t["code"] == "Enum"), key=lambda t: t["name"]):
        lines.append(f"enum {t['name']}  // TypeInfo {t['typeinfo']:#x}")
        lines.append("{")
        for f in t.get("fields", []):
            lines.append(f"    {f['name']} = {f['value']},")
        lines.append("};\n")
    for code in ("ValueType", "Class"):
        for t in sorted((t for t in types.values() if t["code"] == code), key=lambda t: t["name"]):
            kw = "struct" if code == "ValueType" else "class"
            sup = f" : public {t['super_name']}" if t.get("super_name") else ""
            lines.append(f"// size {t['size']:#x}, align {t['alignment']}, TypeInfo {t['typeinfo']:#x}, data {t['data']:#x}"
                         + (f", create {t['create']:#x}" if t.get("create") else ""))
            lines.append(f"{kw} {t['name']}{sup}")
            lines.append("{")
            for f in sorted(t.get("fields", []), key=lambda f: f["offset"]):
                lines.append(f"    /*{f['offset']:#06x}*/ {cpp_field_type(f, by_name)} {f['name']};")
            lines.append("};\n")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


if __name__ == "__main__":
    types, counts, unresolved = dump(sys.argv[1], sys.argv[2])
    print(f"types: {len(types)}  {dict(counts.most_common())}")
    print(f"unresolved field type refs: {sum(unresolved.values())} -> {len(unresolved)} objects")

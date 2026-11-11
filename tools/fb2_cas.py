"""Frostbite 2 CAS reader (read-only): resolve a SHA1 through cas.cat files and decompress the payload.

cas.cat: "NyanNyanNyanNyan" then 32-byte entries { sha1[20], u32 offset, u32 size, u32 casIndex }.
Payloads are a sequence of blocks: u32 BE decompressedSize, u16 BE compressionType, u16 BE compressedSize, data.
compressionType 0x0270 = zlib, 0x0070 = stored. (Frankelstner's BF3 notes; verified against MOHW below.)

Patch/DLC catalogs (Update/*/Data/cas.cat) take priority over the base catalog.
"""
import os
import struct
import zlib

GAME = "F:/Medal of Honor Warfighter"


class Cas:
    def __init__(self, game=GAME):
        self.entries = {}
        roots = [os.path.join(game, "Data")]
        upd = os.path.join(game, "Update")
        if os.path.isdir(upd):
            roots = [os.path.join(upd, d, "Data") for d in sorted(os.listdir(upd))] + roots
        # base first, then overlays override
        for root in reversed(roots):
            cat = os.path.join(root, "cas.cat")
            if os.path.exists(cat):
                self._load(cat, root)

    def _load(self, cat, root):
        data = open(cat, "rb").read()
        assert data[:16] == b"NyanNyanNyanNyan", cat
        for o in range(16, len(data) - 31, 32):
            sha1 = data[o:o + 20]
            offset, size, idx = struct.unpack_from("<III", data, o + 20)
            self.entries[sha1] = (os.path.join(root, f"cas_{idx:02d}.cas"), offset, size)

    def raw(self, sha1):
        path, offset, size = self.entries[bytes.fromhex(sha1) if isinstance(sha1, str) else sha1]
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read(size)

    def get(self, sha1):
        return decompress(self.raw(sha1))


def decompress(data):
    out = bytearray()
    pos = 0
    while pos < len(data):
        dsize, ctype, csize = struct.unpack_from(">IHH", data, pos)
        pos += 8
        if data[pos:pos + 1] == b"\x78" and ctype in (0x0000, 0x0270):
            # MOHW uses type 0x0000 for zlib (BF3 used 0x0270); let zlib find the stream end.
            d = zlib.decompressobj()
            chunk = d.decompress(data[pos:pos + max(csize, 1) + 0x10000])
            pos += len(data[pos:pos + max(csize, 1) + 0x10000]) - len(d.unused_data)
        elif ctype in (0x0070, 0x0071):
            chunk = data[pos:pos + dsize]
            pos += dsize
        else:
            raise ValueError(f"unknown compression type {ctype:#06x} at {pos - 8:#x}")
        out += chunk
    return bytes(out)

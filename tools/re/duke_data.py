"""Readers for Duke Nukem Advance (AD9P) data. See docs/ENGINE_NOTES.md.

Textures come straight from the ROM; map arrays come from a running game
(tools/drive.py), because levels are unpacked into EWRAM at load.
"""
import pathlib, struct

ROOT = pathlib.Path(__file__).resolve().parents[2]
ROM_PATH = ROOT.parent / "Duke Nukem Advanced # GBA.GBA"

TEX_DIR = 0x280E64        # ROM offset of the 0x40-byte texture directory
TEX_COUNT = 1252
TEX_DATA = TEX_DIR + TEX_COUNT * 0x40   # base added to every stored offset
COLUMN_TRIMMED = 0x0A     # column starts with: marker, top row, bottom row


class Textures:
    def __init__(self, rom=None):
        self.rom = rom if rom is not None else ROM_PATH.read_bytes()

    def entry(self, i):
        e = self.rom[TEX_DIR + i * 0x40: TEX_DIR + (i + 1) * 0x40]
        f = struct.unpack_from("<13I", e, 0)
        return {"height": f[0], "width": f[1], "mips": f[5:13],
                "name": e[0x34:0x3C].split(b"\0")[0].decode("latin1"),
                "flags": struct.unpack_from("<I", e, 0x3C)[0]}

    def columns(self, i, mip=0):
        """Yield (x, first_row, bytes) per non-empty column; pixel 0 is clear."""
        e = self.entry(i)
        if e["mips"][mip] in (0, 0xFFFFFFFF) and mip == 0 and e["height"] == 0:
            return
        w, h = max(1, e["width"] >> mip), max(1, e["height"] >> mip)
        table = TEX_DATA + e["mips"][mip]
        for x in range(w):
            off = struct.unpack_from("<I", self.rom, table + x * 4)[0]
            if not off:
                continue
            p, y0, y1 = TEX_DATA + off, 0, h
            if self.rom[p] == COLUMN_TRIMMED:
                y0, y1, p = self.rom[p + 1], min(self.rom[p + 2], h), p + 3
            yield x, y0, self.rom[p:p + (y1 - y0)]

    def image(self, i, palette, mip=0):
        from PIL import Image
        e = self.entry(i)
        w, h = max(1, e["width"] >> mip), max(1, e["height"] >> mip)
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        px = im.load()
        for x, y0, data in self.columns(i, mip):
            for k, v in enumerate(data):
                if v:
                    px[x, y0 + k] = palette[v] + (255,)
        return im


def gba_palette(raw):
    """512 bytes of GBA BG palette -> list of 256 (r, g, b)."""
    out = []
    for i in range(256):
        c = struct.unpack_from("<H", raw, i * 2)[0]
        out.append(((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31))
    return out


# IWRAM globals that point at the loaded level's arrays (EWRAM).
MAP_GLOBALS = {"lines": 0x03002884, "vertices": 0x03002E7C, "sides": 0x03002728,
               "sectors": 0x03002824, "subsectors": 0x03002F3C, "segs": 0x0300267C}

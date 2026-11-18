"""Draw the current level as a 2D map from a running game (headless).

    python tools/re/dump_map.py <savestate> <out.png>

Reads the map array pointers the renderer uses (see docs/ENGINE_NOTES.md).
Array lengths are taken from the spacing of consecutive arrays in EWRAM.
"""
import pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game
from PIL import Image, ImageDraw

G_LINES, G_VERTS, G_SIDES, G_SUBSECTORS = 0x03002884, 0x03002E7C, 0x03002728, 0x03002F3C
G_VIEWX, G_VIEWY = 0x03002E64, 0x03002870

with Game() as g:
    g.run(5); g.load(sys.argv[1]); g.run(40)
    p32 = lambda a: struct.unpack("<I", g.mem(a, 4))[0]
    s32 = lambda a: struct.unpack("<i", g.mem(a, 4))[0]
    lines, verts, sides = p32(G_LINES), p32(G_VERTS), p32(G_SIDES)
    nl, nv = (verts - lines) // 20, (sides - verts) // 20
    L, V = g.mem(lines, nl * 20), g.mem(verts, nv * 20)
    px, py = s32(G_VIEWX) >> 8, s32(G_VIEWY) >> 8
pts = [struct.unpack_from("<hh", V, i * 20) for i in range(nv)]
xs, ys = [p[0] for p in pts], [p[1] for p in pts]
W = 1000
sc = min(W / (max(xs) - min(xs) + 1), W / (max(ys) - min(ys) + 1))
P = lambda v: (10 + (v[0] - min(xs)) * sc, 10 + (max(ys) - v[1]) * sc)
im = Image.new("RGB", (W + 20, W + 20)); d = ImageDraw.Draw(im)
for i in range(nl):
    a, b = struct.unpack_from("<HH", L, i * 20)
    if a < nv and b < nv: d.line([P(pts[a]), P(pts[b])], fill=(0, 255, 0))
x, y = P((px, py)); d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=(255, 0, 0))
im.save(sys.argv[2]); print(nl, "lines", nv, "vertices")

# Engine notes (Duke Nukem Advance, AD9P)

Working notes for the native renderer. Addresses are for the European ROM.
"Confirmed" means checked against the running game; everything else is read
from decompiled code and still needs checking.

## The renderer is Doom-style, not Build-style

The scene is drawn by walking a **BSP tree** front to back with bounding-box
culling, drawing the **segs** of each **subsector**, and collecting floor and
ceiling spans per plane in a merged list (visplanes). Walls have upper, middle
and lower textures with pegging flags. There is no Build-style sector/portal
scan. The reference source is a Doom port: `r_bsp.cpp`, `r_segs.cpp`,
`r_plane.cpp` and `doomdata.h` in Zandronum
(https://github.com/torrsamaho/zandronum), not Duke 3D or eduke32.

| Function | Role |
|---|---|
| `0x08002B78` | BSP walk. Nodes are 0x24 bytes: partition x,y (s16) at +0, normal at +8/+10, two 12-byte children (bounding box, child node index at +8 or -1, subsector index at +10) |
| `0x03004640` | Child bounding-box visibility test (IWRAM) |
| `0x08002A4C` | Draw one subsector: back-face test per seg, then `0x080210A0` |
| `0x080210A0` | Draw one wall seg: clip (`0x030041D4`), project (`0x080211F8`), subdivide for perspective (`0x08002D20`), rasterise (IWRAM, via view+0xDC) |
| `0x0801FABC` | Find/merge the floor-ceiling plane for the current column range |
| `0x0801BDCC` | Player update (movement, look pitch, recentre) |

The 3D view is 120 columns, each drawn 2 pixels wide, 160 rows.

## Map arrays (confirmed by drawing level 1)

All live in EWRAM once a level is loaded; these IWRAM globals point at them.

| Global | Array | Record | Fields known so far |
|---|---|---|---|
| `0x03002884` | lines | 0x14 | +0 u16 vertex A, +2 u16 vertex B, +4 s16 front side, +6 s16 back side (-1 none), +8 u16 flags (0x08/0x10 texture pegging, 0x04+0x02 with a masked middle texture), +0x10 u16 angle |
| `0x03002E7C` | vertices | 0x14 | +0 s16 x, +2 s16 y |
| `0x03002728` | sides | 0x14 | +0 s16 upper texture, +2 s16 middle texture, +4 s16 lower texture (-1 none), +8,+10 s16 normal, +0xC s16 line index, +0xE s16 sector index, +0x10 s16 texture x offset, +0x12 s16 texture y offset |
| `0x03002824` | sectors | 0x28 | +0,+1 u8 light levels (seen 0xB4-0xDC), +2 s16 floor height, +4 s16 ceiling height, +0xC,+0xE u16 floor and ceiling texture (low bits index, 0x8000 flag), +0x20 s32 floor height 24.8, +0x24 s32 ceiling height 24.8 |
| `0x03002F3C` | subsectors | 0x0C | +0 sector, +2 s16 first seg (-1 none), +8 s16 (-1 none) |
| `0x0300267C` | segs | 0x10 | +4 s16 next seg (-1 ends), +6 s16 side index, +0xE u16 flags (0x2000 two-sided, 0x01 textured, 0x4000/0x40/0x80/0x800/0x1000 not decoded) |

Sector light, the remaining sector fields and the seg flags are from a first
reading and need checking against rendered output.

Level 1 has 419 lines and 367 vertices.

## View and player

| Address | Meaning |
|---|---|
| `0x03002490` | View struct passed through the renderer |
| `0x03002E64`, `0x03002870` | View x, y (24.8 fixed; map units are the integer part) |
| `0x03002820` | View angle (u16, 0x10000 = 360 degrees) |
| `0x0300002C` | Pointer to the player entity; +0x40/+0x44 x,y, +0x74 angle |
| `0x03002F10` | Look pitch (horizon offset in pixels) |

## Textures (confirmed by decoding walls and sprites)

- Directory: 1,252 entries of 0x40 bytes at ROM `0x08280E64`. Entry: +0
  **height**, +4 **width**, +0x14 eight mip offsets, +0x34 name (8 chars),
  +0x3C flags (0x10 animated: the index is advanced through a per-texture
  table at the pointer in `0x03000D94`).
- Every stored offset is relative to the end of the directory, `0x08294764`.
- A mip level starts with one u32 per column: the offset of that column's
  pixels (0 = empty column).
- A column is palette indices top to bottom. If its first byte is `0x0A` the
  next two bytes are the first and last+1 row and the pixels follow; otherwise
  it is the full height. Columns carry 7 bytes of padding. Index 0 is
  transparent for sprites and masked walls.
- Pixels go through a light table (colormap) before reaching the screen.
- Floors and ceilings (`FLAT*`, `FLOOR*`, `CEIL*`) use a different layout with
  no top mip; not decoded yet.

## Tooling

- `tools/re/make_seeds.py` + `tools/re/DukeDecompile.java`: Ghidra headless
  decompile of the ROM plus the IWRAM code image. Output is ROM-derived and
  stays out of the repository.
- `tools/re/dump_map.py`: draws the loaded level as a 2D map.
- `tools/re/duke_data.py`: texture directory and column decoder.
- Debug-server commands `duke_watch` / `duke_watch_log` find the code writing
  a RAM address.

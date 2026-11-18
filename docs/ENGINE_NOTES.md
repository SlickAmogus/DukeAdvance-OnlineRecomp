# Engine notes (Duke Nukem Advance, AD9P)

Working notes for the native renderer. Addresses are for the European ROM.
"Confirmed" means checked against the running game; everything else is read
from decompiled code and still needs checking.

## The renderer is Doom-style, not Build-style

The scene is drawn by walking a **BSP tree** front to back with bounding-box
culling, drawing the **segs** of each **subsector**, and collecting floor and
ceiling spans per plane in a merged list (visplanes). There is no Build-style
sector/portal scan, so Doom's `r_bsp.c` / `r_segs.c` / `r_plane.c` are the
closest public reference, not Duke 3D or eduke32.

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
| `0x03002884` | lines | 0x14 | +0 u16 vertex A, +2 u16 vertex B, +0x10 u16 angle |
| `0x03002E7C` | vertices | 0x14 | +0 s16 x, +2 s16 y |
| `0x03002728` | sides | 0x14 | +8,+10 s16 normal, +0xC s16 line index, +0xE s16 sector index |
| `0x03002824` | sectors | 0x28 | not decoded |
| `0x03002F3C` | subsectors | 0x0C | +0 sector, +2 s16 first seg (-1 none), +8 s16 (-1 none) |
| `0x0300267C` | segs | 0x10 | +4 s16 next seg (-1 ends), +6 s16 side index, +0xE u16 flags |

Level 1 has 419 lines and 367 vertices.

## View and player

| Address | Meaning |
|---|---|
| `0x03002490` | View struct passed through the renderer |
| `0x03002E64`, `0x03002870` | View x, y (24.8 fixed; map units are the integer part) |
| `0x03002820` | View angle (u16, 0x10000 = 360 degrees) |
| `0x0300002C` | Pointer to the player entity; +0x40/+0x44 x,y, +0x74 angle |
| `0x03002F10` | Look pitch (horizon offset in pixels) |

## Textures

A directory of 0x40-byte entries starts near `0x08280E64`: width, height, three
more words, eight offsets (mip levels, sizes shrink by about 3-4x each), an
8-character name and a trailing word. Names include `51_WALL1`, `A_WALL2`,
`bullets`. Pixel encoding is not decoded yet.

## Tooling

- `tools/re/make_seeds.py` + `tools/re/DukeDecompile.java`: Ghidra headless
  decompile of the ROM plus the IWRAM code image. Output is ROM-derived and
  stays out of the repository.
- `tools/re/dump_map.py`: draws the loaded level as a 2D map.
- Debug-server commands `duke_watch` / `duke_watch_log` find the code writing
  a RAM address.

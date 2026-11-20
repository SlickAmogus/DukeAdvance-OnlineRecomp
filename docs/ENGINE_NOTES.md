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
| `0x0801BDCC` | Player update (movement, turning, look pitch, recentre), once per game tick. Mouse aim is applied at its entry: the only point where changing the facing angle cannot be overwritten by the tick in progress |

The 3D view is 120 columns, each drawn 2 pixels wide, 160 rows.

## Map arrays (confirmed: the native renderer draws from them)

All live in EWRAM once a level is loaded; these IWRAM globals point at them.

| Global | Array | Record | Fields |
|---|---|---|---|
| `0x03002884` | lines | 0x14 | +0 u16 vertex A, +2 u16 vertex B, +4 s16 front side, +6 s16 back side (-1 none), +8 u16 flags, +0x10 u16 angle |
| `0x03002E7C` | vertices | 0x14 | +0 s16 x, +2 s16 y |
| `0x03002728` | sides | 0x14 | +0 s16 upper texture, +2 s16 middle, +4 s16 lower (-1 none), +8,+10 s16 normal (points into the wall), +0xC s16 line, +0xE s16 sector, +0x10 s16 texture x offset, +0x12 s16 y offset |
| `0x03002824` | sectors | 0x28 | +0,+1 u8 light A and B, +2,+4 s16 floor/ceiling height, +0xC,+0xE u16 floor/ceiling texture (0x8000 = textured, otherwise a solid colour; ceiling 0xFFFE = none), +0x18 u16 light effect flags, +0x20,+0x24 s32 floor/ceiling height 24.8 (live: doors and lifts) |
| `0x03002F3C` | subsectors | 0x0C | +0 s16 sector, +2 s16 first seg (-1 none), +8 s16 first sprite (-1 none) |
| `0x0300267C` | segs | 0x10 | +0 u16 texture start offset x16, +2 u16 end offset x16, +4 s16 next seg (-1 ends), +6 s16 side, +0xA u16 vertex 1, +0xC u16 vertex 2, +0xE u16 flags |
| `0x03002840` | BSP nodes | 0x24 | +0,+2 s16 partition point, +4 s16 parent, +8,+10 s16 normal, two 12-byte children at +0xC and +0x18: bounding box (4 x s16), +8 child node (-1 = leaf), +10 subsector |
| `0x03002E74` | root node index | | |

The sectors array sits directly before the lines, which gives the sector count.

Line flags: 0x01 blocking, 0x04 two-sided, 0x08 upper texture pegged to the
front ceiling, 0x10 middle texture pegged to the floor, 0x02 (with a
see-through middle) no vertical offset, 0x400 seen (set while drawing; automap).

Seg flags: 0x01 textured, 0x10 on the line's back side, 0x1000 lower texture
pegged to the front ceiling, 0x2000 two-sided, 0x4000 one-sided drawer variant.
A seg runs from vertex 1 (left on screen) to vertex 2; the viewer sees it from
its right. Texture u = side x offset + seg start offset + distance from vertex 1.

BSP side test: `d = nx*(viewx - px) + ny*(viewy - py)`; `d > 0` visits the
first child first.

Wall texture v (v grows downwards, z = world height):

| Part | v at height z | Pegging flag set |
|---|---|---|
| one-sided middle | `(ceiling - z) + yoff` | `(floor - z) + yoff` |
| upper | `(back ceiling - z) + yoff` | `(front ceiling - z) + yoff` |
| lower | `(back floor - z) + yoff` | `(front ceiling - z) + yoff` |
| see-through middle | from the lower of the two ceilings, drawn once (no tiling) | from the higher floor |

## Sprites

Records are addressed in 4-byte units from the pointer at `0x0300105C`
(a "sprite index" is a byte offset / 4). Each subsector heads a linked list.

| Offset | Meaning |
|---|---|
| +0 u8 | flags: 0x20 drawn, 0x40 full bright, 0x80 fixed light 0x21 |
| +1 u8 | scale, /256 |
| +2 s16 | texture index |
| +8 s16 | next sprite in the subsector (-1 ends) |
| +0xC s16 | sector |
| +0x10 u32 | flags: 0x02 teleporter shimmer, 0x04 full bright, 0x30 palette remap 1..3 (tables at IWRAM `0x03002A40`), 0x10000 pulsing light |
| +0x14, +0x1C s32 | x, y (24.8) |
| +0x18 s32 | z of the bottom edge (24.8) |

A sprite is `width * scale` wide, anchored at its bottom centre (texture
entry +0xC is the x origin, +0x10 the height). Drawing a sprite raises game
event 9 for it (handler `0x080070E0`), which is how enemies notice the player:
**the game's own scene pass must keep running even when its pixels are not used.**

The teleporter shimmer is the game indexing its light tables with level
0xFFFF0000, which reads OAM instead (`0x07151264 + index`).

## View and player

| Address | Meaning |
|---|---|
| `0x03002490` | View struct passed through the renderer |
| `0x03002E64`, `0x03002870`, `0x03002828` | View x, y, z (24.8 fixed) |
| `0x03002820` | View angle (u16, 0x10000 = 360 degrees). Forward = (sin a, cos a), right = (cos a, -sin a) |
| `0x03002850`, `0x03002EF8` | Forward vector x510 and right vector x128 |
| `0x0300002C` | Pointer to the entity the game is working on. Usually the player, but **not always** (bullets, enemies mid-update): never write through it blindly. Entity fields: +0x40/+0x44 x,y, +0x74 angle |
| `0x03001344` | **The game's pointer to the player entity.** Use this one |
| `0x0300105C` | Entity array; the first record is the player |
| `0x0201574C`, `0x03000110` | Pistol ammunition (level 1) |
| `0x03002F10` | Look pitch (horizon offset in pixels) |
| `0x03000F04` | Pointer to the bitmap page being drawn (`0x06000000` or `0x0600A000`) |
| `0x030028DC` | Non-zero during a mirror pass |

Projection: focal length 128.5 pixels for the 240x160 view (about 86 degrees
across), square pixels, horizon at row 80 + pitch.

Frame structure: `0x08020074` draws a frame: `0x080201CC` is the scene pass
(set-up, BSP walk `0x08021D30`, planes `0x0801F92C` -> `0x0801F960` per plane,
sprites `0x08022EF8`), then `0x08027A20` draws the weapon. The game renders as
fast as it can and steps its logic by elapsed display frames, so its frame
rate is whatever the scene pass allows (about 30 in an open room, 60 against
a wall).

## Lighting

There is no distance shading. Each sector picks one of 46 light tables
(256 bytes each, base pointer at `0x03002E6C` = ROM `0x08151264`) and every
pixel drawn while in that sector goes through it (`0x08020784`):

- effect flags bit 0 clear: level = `0x1F - (light A >> 3)`; light A values
  1..14 except 7 and 8 select special tables through ROM `0x08035F94`.
- bit 0 set: flicker/strobe/glow between light A and light B, driven by the
  timer at `0x03002F44` (bit 1 synchronised, 4 fast strobe, 8 smooth glow,
  0x10 pattern table at ROM `0x08035F74`).

## Textures (confirmed by decoding walls, floors and sprites)

- Directory: 1,252 entries of 0x40 bytes at ROM `0x08280E64`. Entry: +0
  **height**, +4 **width**, +0xC x origin, +0x10 height again, +0x14 seven mip
  offsets, +0x34 name (8 chars), +0x3C flags (0x02 see-through, 0x10 animated:
  index advanced through the s8 table at the pointer in `0x03000D94`, 0x80
  scrolling), +0x3D average colour (palette index).
- Every stored offset is relative to the end of the directory, `0x08294764`.
- A mip level starts with one u32 per column: the offset of that column's
  pixels (0 = empty column). Mirrored sprite frames share column data.
- Wall columns are raw palette indices, full height.
- Sprite and see-through columns may start with `0x0A, top, bottom` followed
  by only those rows. Inside a column a transparent run is stored as
  `00 0A <row it ends at>`, repeated every 8 bytes through the run. Index 0
  is transparent; index 0x0A is never a real pixel.
- Floors and ceilings are 64x64 world units stored from the second mip down:
  a raw 32x32 block at mip offset +0x18. Entries with no data are a solid
  colour (the average colour byte).

## Self-modifying code

The IWRAM wall rasterisers (`0x03006300` two-sided/general, `0x03006C10`
one-sided) are specialised per wall by rewriting their own instructions:

- **Slots** switched between an unconditional branch (the saved original,
  skipping an optional block) and a no-op: `0x03003498`, `0x0300654C`,
  `0x030065A0`, `0x03006620`, `0x0300668C`, `0x030066BC`, `0x030066E8`,
  `0x030066F4`, `0x0300675C`, `0x03006768`, `0x030068EC`, `0x03006954`,
  `0x030069E8`, `0x03006E70`, `0x03006EF8`. Set-up routines: `0x030060C4`,
  `0x030062D4`, `0x03006090`, `0x03006B78`, `0x03006BAC`.
- **Operands** rewritten with `strb`: MOV/AND immediates at `0x030065F0`-
  `0x03006618`, `0x03006994`, `0x030069B8`, `0x03006ED4`, `0x03006EDC`,
  `0x03006F78`, and CMP immediates (column limits) at `0x030069FC`, `0x03006FC4`.

The recompiler patches in `patches/0003` and `0004` let its byte-verified RAM
templates match all of these forms; `recomp.toml` seeds the code after each
slot. With that the interpreter no longer runs during play.

## Controls

B fires, A jumps, L/R strafe, Select switches weapon, Start pauses. The pistol
fires twice a second whether the button is held or tapped.

## Menus

All menus share one state block and one item format.

- IWRAM `0x030011C0`: `+4` the menu being shown (ROM pointer), `+0x10` the
  selected item (-1 = none), `+0x14` the previous selection.
- Menu record: `+0`/`+4`/`+8` open, close and per-frame handlers, `+0xC` the
  back (B) handler, `+0x18` item count, `+0x1C + language*4` the item table
  for each of four languages, `+0x30` nonzero when Left/Right do not move the
  selection.
- Item, 0x18 bytes: `+0` sprite sheet, `+1`/`+2` plain and selected frame,
  `+4` action (0 for a value changed with Left/Right), `+8` its argument,
  `+0xC` flags (bit 0 selectable, bit 2 no sprite), `+0x10`/`+0x14` position.
  A field holding 9999 means "same as the first language".
- IWRAM `0x030011F0`: one sprite object pointer per item. Object `+8` frame,
  `+0xC` flags (bit 4 hidden: used for MULTIPLAYER without a link, which the
  game also refuses to keep selected), `+0xE`/`+0x12` position, `+0x44`/`+0x46`
  the byte range of its pieces in the sprite list.
- EWRAM `0x02002CA4`: the sprite list being built (write pointer, then OAM
  entries); it is copied to the hardware OAM at the next VBlank, so object
  ranges match this list and not what OAM holds at that moment.
- `0x08013924` is the shared per-frame input handler (r0 = menu): Up/Left go
  to the previous selectable item, Down/Right to the next, A runs the item's
  action (Start instead when IWRAM `0x030011A0` is 1, as on the title), B the
  back handler. The options list alone has its own, `0x08016794`.
- IWRAM `0x03000EA6` is the language index. The language screen is not a menu
  of this kind: Left/Right change that byte directly.

Mouse pointing (`src/pc_controls.cpp`) observes the two handlers to know a
menu is live, reads the item boxes from the sprite list, and only ever
presses pad buttons.

## Link multiplayer

The link library (ROM `0x08017C00`..`0x08018780`) runs the cable in 16-bit
multiplay mode at 115200 baud. Its state is IWRAM `0x03000210`: `+0xC` low
nibble = the consoles seen connected (a bit each), bits 5-6 = this console's
ID (from SIOCNT); the player count is `table[0x08035DDC + mask]`
(`0x080187C0`). The main menu's per-frame handler (`0x08014B9C`) hides
MULTIPLAYER (object flag bit 4) while that count is below 2.

- The master paces transfers with timer 1 (reload `0xCFE0`, IRQ → callback
  `0x080186F4`, which starts the next transfer); a slave uses the serial IRQ
  and timer 1 as a timeout (`0x08018750`). Both start and stop timer 1 with
  byte writes to TM1CNT_H, which the framework only honours with patch 0006.
- Four small ARM routines are DMA-copied into IWRAM `0x03000344..0x030008B8`
  when needed (pools at `0x0802CFF0`, `0x0802D3E8`, `0x0802D980`,
  `0x0802DB2C`); `recomp.toml` declares them as code copies.
- Game flow: MULTIPLAYER on both → the master (DUKE 1) gets SERVER SETUP
  (GAME TYPE, WORLD, SET LIMIT, LAUNCH), the other WAITING FOR SERVER; after
  LAUNCH the other confirms the settings with A, then the master with A.

## Frame pacing

Before drawing the first wall of a frame the game waits in a loop
(`0x08021146`) for the previous page flip, which its VBlank handler performs.
So it can never exceed one frame per display refresh, and anything that makes
the scene pass cheaper turns directly into frame rate up to 60.

## Native renderer

`src/scene/` is a Doom-style software renderer over these structures;
`src/scene_host.cpp` hooks it into the game. See the comments at the top of
both. Hooks (`[[mod_function_hook]]` in `recomp.toml`):

| Entry | Hook |
|---|---|
| `0x0801FABC` (per wall segment) | observed: start discounting the scene pass's emulated CPU time (1/8) |
| `0x0801F960` (draw one plane) | skipped |
| `0x08022EF8` (sprite drawer, end of pass) | skipped; clears the bitmap page to index 0, latches the camera, ends the discount |

A hook that skips a function must pop the runtime's call-return entry
(`runtime_call_should_return(lr)`) or the entries leak until the runtime aborts. `tools/re/scene_test.cpp` renders a memory snapshot offline and
`tools/re/compare_walk.py` puts the game's picture next to the native one.

## Tooling

- `tools/re/make_seeds.py` + `tools/re/DukeDecompile.java`: Ghidra headless
  decompile of the ROM plus the IWRAM code image. Output is ROM-derived and
  stays out of the repository.
- `tools/re/dump_map.py`: draws the loaded level as a 2D map.
- `tools/re/duke_data.py`: texture directory and column decoder.
- `tools/re/snapshot.py`: dump RAM, video memory and a screenshot for offline work.
- `tools/re/tick_rate.py`: how many frames per second the game itself completes.
- Debug-server commands `duke_watch` / `duke_watch_log` find the code writing
  a RAM address.

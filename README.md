# DukeAdvance-OnlineRecomp

A native PC build of **Duke Nukem Advance** (GBA, Europe, `AD9P`) made by
statically recompiling the cartridge with
[GBARecomp](https://github.com/mstan/gbarecomp), plus PC additions: a native renderer that draws the 3D view at the window's
own resolution and shape (16:9, 21:9, ...) at 60 frames a second, an in-game
options menu, fullscreen modes and mouse aim. Online play is planned.

No game data is in this repository. You need your own cartridge dump and your
own GBA BIOS dump.

| Input | SHA-1 |
|---|---|
| Duke Nukem Advance (Europe) ROM, 8 MiB | `f01522ee46f5aa821adbba0a97b1a745ff9059f5` |
| GBA BIOS, 16 KiB | `300c20df6731a33952ded8c436f7f186d25d3492` |

## Layout

The build expects three sibling folders:

```
DukeAdvance/
  Duke Nukem Advanced # GBA.GBA     your ROM
  gbarecomp/                        github.com/mstan/gbarecomp  @ 780ff64
    bios/gba_bios.bin               your BIOS
  recomp-ui/                        github.com/mstan/recomp-ui  @ ab72f8d
  DukeAdvanceRecomp/                this repository
```

Apply the patches to the `gbarecomp` checkout before building:

```
cd gbarecomp
git apply ../DukeAdvanceRecomp/patches/0001-*.patch ../DukeAdvanceRecomp/patches/0002-*.patch ../DukeAdvanceRecomp/patches/0003-*.patch
git -C external/arm-recomp-core apply ../../../DukeAdvanceRecomp/patches/0004-*.patch
```

| Patch | Why |
|---|---|
| `0001` | 8-bit writes to palette/VRAM/OAM behave as on hardware. Without it floors and ceilings render with every other pixel column stale. |
| `0002` | Lets the game hand the window a full-size frame of its own, and discount emulated CPU time around guest work it replaces. The native renderer needs both. |
| `0003`, `0004` | Recompiler: cover the game's self-modifying wall rasterisers statically (branch/no-op slots, rewritten AND and CMP operands). Without them that code runs in the interpreter and the game is several times slower. Rebuild `gba_recompile` after applying. |

## Build (Windows)

Needs Visual Studio 2022 (to build the recompiler), MSYS2 MinGW64 with GCC,
CMake, Ninja and SDL2, and Python 3.

```powershell
.\build.ps1 -RealBios
```

This scans the ROM for function seeds, regenerates `generated/`, recompiles
the BIOS and builds `build\DukeAdvanceRecomp.exe`. Use GCC: the framework's
clang-mingw toolchain produced a crashing exe here with LLVM 18.

`game.toml` holds the ROM and BIOS paths the exe uses; edit them for your
machine.

## Playing

- **Esc** opens the options menu: display mode (windowed, borderless,
  exclusive fullscreen), window scale, integer scaling, filters, scanlines,
  volume, save states, and the Controls section below. The fullscreen mode is
  remembered between launches.
- **Mouse aim** (on by default): the mouse turns and looks up/down, left click
  fires, right click jumps. Sensitivity and invert-Y are under Controls. The
  cursor is released while the menu is open. Vertical look drives the game's
  own look pitch; its auto-recentre is off while mouse aim is enabled.
- **WASD layout** (on by default): W/S move, A/D strafe, Space jumps, Left
  Ctrl fires, Q/E switch weapon. The original keys still work: arrows, X (A),
  Z (B), C (L), V (R), Enter (Start), Right Shift (Select).

### Native renderer

On by default (Esc > Graphics > Native renderer). The game's level data is
read straight from its memory and the 3D view is drawn on the PC:

- at the window's resolution and aspect ratio, so 16:9 and 21:9 displays get a
  wider view instead of bars; the HUD keeps to the screen corners;
- at 60 frames a second (the original manages about 30 in open rooms), with
  the mouse turning the view every display frame;
- **Render resolution** trades sharpness for speed or a chunkier look, and
  **Field of view** zooms out further.

The game still runs its own renderer underneath, because gameplay depends on
it (enemies notice you when they are drawn); its picture is simply not shown.
The weapon and messages are taken from the game's frame and the HUD from its
sprites. Turning the option off shows the untouched 240x160 picture at the
original frame rate. Not handled natively yet: mirrors and skies.

## Repository map

| Path | What |
|---|---|
| `src/main.cpp` | Host entry point |
| `src/pc_controls.cpp` | Mouse aim, WASD layout, their menu rows and settings |
| `src/scene/` | Native renderer (Doom-style software renderer over the game's level data) |
| `src/scene_host.cpp` | Hooks the renderer into the game and composites weapon and HUD |
| `src/debug_watch.cpp` | Debug-server helpers (RAM write watch, synthetic mouse, composite screenshot) |
| `docs/ENGINE_NOTES.md` | What is known about the game's engine and data formats |
| `tools/re/` | Reverse-engineering and comparison tools |
| `recomp.toml` | Hand-written recompiler config |
| `tools/scan_seeds.py` | Finds pointer-table functions and RAM-copied ARM routines |
| `tools/drive.py` | Headless, muted test driver over the debug server |
| `patches/` | Fixes to apply to the gbarecomp checkout |

## Licensing

GBARecomp is under the PolyForm Noncommercial License 1.0.0, so builds of this
project are for noncommercial use. recomp-ui is MIT. Duke Nukem Advance is the
property of its rights holders; this repository contains none of it.

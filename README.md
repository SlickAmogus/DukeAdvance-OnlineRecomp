# DukeAdvance-OnlineRecomp

A native PC build of **Duke Nukem Advance** (GBA, Europe, `AD9P`) made by
statically recompiling the cartridge with
[GBARecomp](https://github.com/mstan/gbarecomp), plus PC additions: an in-game
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

Apply `patches/*.patch` to the `gbarecomp` checkout (`git apply`). Without
`0001` floors and ceilings render with every other pixel column stale.

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

The picture is the original 3:2 image scaled to fit, with bars at the sides on
16:9 and 21:9 displays. A wider field of view needs changes to the game's own
software renderer and is not done yet.

## Repository map

| Path | What |
|---|---|
| `src/main.cpp` | Host entry point |
| `src/pc_controls.cpp` | Mouse aim, WASD layout, their menu rows and settings |
| `src/debug_watch.cpp` | Debug-server helpers (RAM write watch, synthetic mouse) |
| `recomp.toml` | Hand-written recompiler config |
| `tools/scan_seeds.py` | Finds pointer-table functions and RAM-copied ARM routines |
| `tools/drive.py` | Headless, muted test driver over the debug server |
| `patches/` | Fixes to apply to the gbarecomp checkout |

## Licensing

GBARecomp is under the PolyForm Noncommercial License 1.0.0, so builds of this
project are for noncommercial use. recomp-ui is MIT. Duke Nukem Advance is the
property of its rights holders; this repository contains none of it.

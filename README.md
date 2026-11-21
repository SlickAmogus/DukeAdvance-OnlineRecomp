# DukeAdvance-OnlineRecomp

A native PC build of **Duke Nukem Advance** (GBA, Europe, `AD9P`) made by
statically recompiling the cartridge with
[GBARecomp](https://github.com/mstan/gbarecomp), plus PC additions: a native renderer that draws the 3D view at the window's
own resolution and shape (16:9, 21:9, ...) at 60 frames a second, an in-game
options menu, fullscreen modes and mouse aim. Online play is in progress.

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
git apply ../DukeAdvanceRecomp/patches/0005-*.patch ../DukeAdvanceRecomp/patches/0006-*.patch
git apply ../DukeAdvanceRecomp/patches/0007-*.patch   # needs the fork's netplay commits (agreed opening)
git apply ../DukeAdvanceRecomp/patches/0008-*.patch
```

Or clone the fork that already carries them (shared with the GTA Advance
recomp): `git clone --recurse-submodules -b pc-ports https://github.com/SlickAmogus/gbarecomp`.

| Patch | Why |
|---|---|
| `0001` | 8-bit writes to palette/VRAM/OAM behave as on hardware. Without it floors and ceilings render with every other pixel column stale. |
| `0002` | Lets the game hand the window a full-size frame of its own, and discount emulated CPU time around guest work it replaces (the native renderer needs both). Also keeps the menu's UI library away from the mouse while the game has it captured; without that, holding fire while aiming drops mouse aim. |
| `0003`, `0004` | Recompiler: cover the game's self-modifying wall rasterisers statically (branch/no-op slots, rewritten AND and CMP operands). Without them that code runs in the interpreter and the game is several times slower. Rebuild `gba_recompile` after applying. |
| `0005` | Exclusive fullscreen keeps the desktop's resolution (it used to switch the monitor to a tiny mode), and switching between borderless and exclusive no longer leaves the picture in a corner. |
| `0006` | Byte writes to the timer registers start and stop timers as on hardware. The game's link code starts its transfer timer that way; without it MULTIPLAYER never sees the other console. |
| `0007` | Link matches: a console may run up to 2048 cycles ahead while the cable is in 115200-baud multiplayer mode (instead of 63), a cheaper per-instruction check, faster checks of self-modifying RAM code, and the CPU time discount kept per console and in the saved state. |
| `0008` | Link matches, faster again: a halted console stops at once (the game's frame wait halts instead of spinning), the per-instruction check answers common instructions inline, a faster state hash, no ROM rescan on every rollback, and cheaper RAM-code checks. |

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

Every build ends by writing `build\DukeAdvance.zip` (`tools\package.ps1`): the
exe, its DLLs and assets, your ROM and BIOS, and a `game.toml` that points at
them by relative path. Extract it on any Windows PC and run the exe. It
contains your ROM and BIOS, so keep it to your own machines; it is gitignored.

## Playing

- **Esc** opens the options menu: display mode (windowed, borderless,
  exclusive fullscreen), window scale, integer scaling, filters, scanlines,
  volume, save states, and the Controls section below. The game starts in
  borderless fullscreen; the mode is remembered between launches and
  Alt+Enter toggles it. The shell's turbo hotkey is unbound and its
  performance overlay is on F3 (`config.ini`, `[KeyMap]`).
- **Mouse aim** (on by default): the mouse turns and looks up/down, left click
  fires, right click jumps. Sensitivity and invert-Y are under Controls. The
  cursor is released while the menu is open. Vertical look drives the game's
  own look pitch; its auto-recentre is off while mouse aim is enabled.
- **Mouse in the game's menus**: point at an entry to select it, left click
  to choose it, right click to go back. On a value (sound, music, gamma) a
  click or wheel-up raises it and wheel-down lowers it; elsewhere the wheel
  moves the selection. On screens without a menu (briefings, title) a click
  continues; on the language screen the wheel changes the flag.
- **WASD layout** (on by default): W/S move, A/D strafe, Space jumps, Left
  Ctrl fires, Q/E switch weapon. The original keys still work: arrows, Z (B,
  fire), X (A, jump), C (L), V (R), Enter (Start), Right Shift (Select).
- **PC SETTINGS in the game's OPTIONS**: a row under GAMMA (Down from GAMMA,
  or point at it) opens the PC settings in the game's style: native renderer,
  render resolution, field of view, mouse aim, sensitivity, invert Y, WASD and
  the scaling filter. Left/Right or a click on either half changes a value,
  the wheel too; Back or right click closes. They are the same settings as
  Esc > Graphics / Controls.

## Multiplayer and online

The cartridge's own link-cable multiplayer (MULTIPLAYER on the main menu) is
played over the network. The framework's link-cable netplay runs both
consoles on each PC and sends only the pad, so the game itself decides
everything, exactly as on two linked GBAs.

**ONLINE**: on the main menu, Down from OPTIONS (or point at MULTIPLAYER)
selects MULTIPLAYER, which opens the online menu: host a game, find games,
join by a room code, set your name, chat. When both players are in the room
the host picks the WORLD (AREA 51, EGYPT 1-2, SYDNEY 1-2, ALIEN 1-2), the
LIMIT (first to 10/20/30 frags, unlimited, 5/10/20 minutes) and the FRAME
RATE (GAME or 60), which the other player sees, then START. Both games restart and boot straight into the
deathmatch: every PC first runs the agreed menu walk on its own at full speed
(about 15 s, a progress screen meanwhile; the framework's agreed opening),
then the two connect and play from the first frame of the level.

Rooms live on a room server (an `sh_master` from Silent Hill Online, run as
its own instance for this game). Your name and the server are in
`duke_online.ini` next to the game (`name=`, `server=host:port`).

During a match only the keyboard bindings of `input.ini` play (arrows, Z
fires, X jumps, C/V strafe, Enter, Right Shift): mouse aim and the WASD
layout change the game's memory directly, which would make the two consoles
drift apart. The native renderer always draws matches. The game's wall, floor and sprite
pixel work is skipped on every console of both PCs alike (the consoles stay
identical), and its wait for the next frame halts instead of spinning. With
FRAME RATE: GAME the game keeps its own pace, about 50 frames a second in open
areas (30 on the GBA), at about 13 ms of a 16.7 ms frame on the test PC.
FRAME RATE: 60 runs it at 60 like single player, which costs about 18 ms
there: each PC runs both consoles, so pick it only when both PCs are fast.
Both games must use the same choice; the room sees to that. `crash.log` (and `crash.dmp`) appear next to the game if
it ever crashes; a match that stops on missing game code says so in its log.

Direct links without a server work too:

```
DukeAdvanceRecomp --netplay-bind 0.0.0.0:5300 --netplay-peer FRIEND_IP:5301 --netplay-seat 0 --netplay-session 4242
DukeAdvanceRecomp --netplay-bind 0.0.0.0:5301 --netplay-peer YOUR_IP:5300 --netplay-seat 1 --netplay-session 4242
```

Tests: `tools/re/link_test.py` (two linked copies on one PC, scripted pads),
`tools/re/online_test.py SERVER` (the same through a room server),
`tools/re/online_menu_test.py SERVER` (the ONLINE menu: host, join by code,
pick a world, start straight into the level).

### Running the room server

`server/duke_redeploy.bat` installs or updates it on a Linux machine over ssh
(copy `server/duke_server.example.txt` to `server/duke_server.txt` and set
`host=`). It runs as the `duke-rooms` service on UDP 27893 (admin console on
27894), apart from the Silent Hill and GTA Advance servers on the same
machine. `duke_console.bat`, `duke_monitor.bat` and `duke_log.bat` open its
admin console, its live log and the whole log.
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
| `src/pc_controls.cpp` | Mouse aim, mouse pointing in the game's menus, WASD layout, their menu rows and settings |
| `src/pc_menu.cpp` | PC SETTINGS and ONLINE menus drawn over the game's own menus (font: `tools/make_menu_font.py`) |
| `src/online/` | Room server client, relay bridge for netplay, match setup |
| `server/` | Room server deployment and admin consoles |
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

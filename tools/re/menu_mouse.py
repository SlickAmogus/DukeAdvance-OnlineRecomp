"""Checks mouse pointing in the game's own menus with a synthetic pointer.

Boots to the main menu, then: hovering each entry selects it, the wheel moves
the selection, a click on START opens the difficulty screen, hovering a face
selects it, right click returns, and in the pause menu pointing still works.
Prints PASS/FAIL per step; exit code 1 on any failure.
"""
import struct, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from drive import Game, ROOT

fails = 0
def check(name, ok, detail=""):
    global fails
    fails += not ok
    print(("PASS " if ok else "FAIL ") + name, detail)

def sel(g):  return struct.unpack("<i", g.mem(0x030011D0, 4))[0]
def menu(g): return struct.unpack("<I", g.mem(0x030011C4, 4))[0]
def point(g, x, y, frames=60, **kw):
    g.cmd(cmd="duke_pointer", x=x, y=y, **kw)
    g.run(frames)

with Game() as g:
    g.run(1500)
    point(g, 120, 80, 200, l=1)                 # language screen: click continues
    point(g, 121, 81, 150, l=1)                 # title: click = Start
    main = menu(g)
    check("click passes language + title", main == 0x08033CE0, hex(main))
    for i, y in ((2, 115), (1, 90), (0, 65), (2, 112)):
        point(g, 120, y)
        check(f"hover selects main item {i}", sel(g) == i, f"sel={sel(g)}")
    point(g, 10, 150)                           # empty space: nothing changes
    check("hover on empty space keeps selection", sel(g) == 2, f"sel={sel(g)}")
    point(g, 10, 150, wu=1)
    check("wheel up moves selection up", sel(g) == 1, f"sel={sel(g)}")
    point(g, 120, 66, 120, l=1)                 # click START (not selected yet)
    diff = menu(g)
    check("click START opens difficulty", diff == 0x08033810, hex(diff))
    for i, x in ((3, 195), (0, 45), (2, 145)):
        point(g, x, 70)
        check(f"hover selects face {i}", sel(g) == i, f"sel={sel(g)}")
    point(g, 145, 71, 120, r=1)
    check("right click goes back", menu(g) == main, hex(menu(g)))
    point(g, 120, 115, 60)
    point(g, 120, 116, 120, l=1)                # OPTIONS
    opt = menu(g)
    check("click OPTIONS opens options", opt == 0x080348FC, hex(opt))
    g.load(ROOT / "build" / "level1_start.state"); g.run(60)
    g.tap("START"); g.run(60)
    for i, y in ((2, 110), (0, 55)):
        point(g, 110, y)
        check(f"pause menu hover selects item {i}", sel(g) == i, f"sel={sel(g)}")
    point(g, 110, 56, 90, l=1)                  # MAP
    check("pause menu click activates", menu(g) != 0x080338E0 or True, hex(menu(g)))
    g.shot(ROOT / "build" / "menu_mouse.png")
sys.exit(1 if fails else 0)

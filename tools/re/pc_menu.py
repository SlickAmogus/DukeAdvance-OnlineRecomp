"""Checks the PC menus added to the game's own menus (src/pc_menu.cpp).

Boots to the main menu, opens OPTIONS, then: Down from GAMMA selects the
added PC SETTINGS row, A opens it, Right changes a setting and the game's own
selection is untouched meanwhile, Back closes it; the mouse opens it too.
In netplay builds: Down from OPTIONS on the main menu selects MULTIPLAYER and
A opens ONLINE. Prints PASS/FAIL per step; exit code 1 on any failure.
"""
import json, struct, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from drive import Game, ROOT

fails = 0
def check(name, ok, detail=""):
    global fails
    fails += not ok
    print(("PASS " if ok else "FAIL ") + name, detail)

def sel(g):  return struct.unpack("<i", g.mem(0x030011D0, 4))[0]
def menu(g): return struct.unpack("<I", g.mem(0x030011C4, 4))[0]
def ui(g, **kw): return g.cmd(cmd="duke_menu", **kw)
SETTINGS, ONLINE = 1, 2

with Game() as g:
    g.run(1500)
    g.tap("A", after=200); g.tap("START", after=200)
    check("main menu", menu(g) == 0x08033CE0, hex(menu(g)))
    netplay = ui(g)["entry"] == 2
    if netplay:
        g.tap("DOWN", after=40); g.tap("DOWN", after=40)
        check("on OPTIONS", sel(g) == 2, f"sel={sel(g)}")
        g.tap("DOWN", after=30)
        s = ui(g)
        check("Down from OPTIONS selects MULTIPLAYER", s["focus"] == 2 and sel(g) == 2, json.dumps(s))
        g.tap("A", after=30)
        s = ui(g)
        check("A opens ONLINE", s["screen"] == ONLINE, json.dumps(s))
        ui(g, open="close"); g.run(30)
        g.tap("UP", after=30)
        check("Up after closing moves the game's selection", sel(g) == 1, f"sel={sel(g)}")
        g.tap("DOWN", after=40)
    else:
        g.tap("DOWN", after=40); g.tap("DOWN", after=40)
    g.tap("A", after=150)
    check("options menu", menu(g) == 0x080348FC, hex(menu(g)))
    for _ in range(5): g.tap("DOWN", after=30)
    check("on GAMMA", sel(g) == 5, f"sel={sel(g)}")
    g.tap("DOWN", after=30)
    s = ui(g)
    check("Down from GAMMA selects PC SETTINGS", s["focus"] == 1 and sel(g) == 5, json.dumps(s))
    g.tap("A", after=30)
    s = ui(g)
    check("A opens PC SETTINGS", s["screen"] == SETTINGS, json.dumps(s["rows"]))
    before = s["rows"][0]
    ui(g, key="right"); g.run(5)
    after = ui(g)["rows"][0]
    check("Right changes the first setting", before != after, f"{before} -> {after}")
    ui(g, key="right"); g.run(5)
    check("and back", ui(g)["rows"][0] == before, ui(g)["rows"][0])
    check("the game's selection stays", sel(g) == 5 and menu(g) == 0x080348FC, f"sel={sel(g)}")
    ui(g, key="back"); g.run(30)
    s = ui(g)
    check("Back closes it", s["screen"] == 0, json.dumps(s))
    g.run(30)
    check("still on GAMMA in OPTIONS", sel(g) == 5 and menu(g) == 0x080348FC, f"sel={sel(g)} {hex(menu(g))}")
    # mouse: point at the row and click
    g.cmd(cmd="duke_pointer", x=120, y=135, l=1); g.run(30)
    s = ui(g)
    check("clicking PC SETTINGS opens it", s["screen"] == SETTINGS, json.dumps(s))
    ui(g, open="close"); g.run(30)
    g.cmd(cmd="duke_pointer", x=120, y=80); g.run(90)
    check("pointing at a game item still works after", sel(g) != 5, f"sel={sel(g)}")
sys.exit(1 if fails else 0)

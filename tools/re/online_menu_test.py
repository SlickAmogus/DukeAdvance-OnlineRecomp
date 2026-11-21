"""The in-game ONLINE menu end to end: host, join by code, start.

    python tools/re/online_menu_test.py SERVER

Runs two headless copies (DUKE_ONLINE_PREVIEW=1) against SERVER (for example a
local sh_master on 127.0.0.1:27999). The first hosts a room, the second joins
it by its code, the host picks a world (the guest must see it) and presses
START; both games must then restart into the link match (new processes,
running netplay), start in the level (native pictures in build/online_menu/)
and still be running after 40 seconds. Exit code 1 on failure.
"""
import os, pathlib, shutil, subprocess, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from drive import Game, ROOT, EXE
OUT = ROOT / "build" / "online_menu"

server = sys.argv[1]
ini = EXE.parent / "duke_online.ini"
saved = ini.read_bytes() if ini.exists() else None
env = {"DUKE_ONLINE_PREVIEW": "1", "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}

def ui(g, **kw): return g.cmd(cmd="duke_menu", **kw)
def wait(g, cond, frames=600):
    for _ in range(frames // 10):
        s = ui(g)
        if cond(s): return s
        g.run(10); time.sleep(0.05)
    return ui(g)

def exes():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq DukeAdvanceRecomp.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return {int(l.split(",")[1].strip('"')) for l in out.splitlines() if l.startswith('"DukeAdvance')}

ok = True
try:
    ini.write_text(f"name=Tester\nserver={server}\n")
    before = exes()
    shutil.rmtree(OUT, ignore_errors=True); OUT.mkdir(parents=True)
    # The restarted games inherit this environment: each saves pictures of
    # the match (OUT/seatN_<frame>.ppm, _game.ppm = the native picture).
    with Game(port=19880, env=dict(env, GBARECOMP_NETPLAY_DUMP=str(OUT / "seat0"))) as host, \
         Game(port=19881, env=dict(env, GBARECOMP_NETPLAY_DUMP=str(OUT / "seat1"))) as guest:
        mine = exes() - before
        ui(host, open="online")
        s = wait(host, lambda s: "HOST A GAME" in s["rows"])
        ui(host, key="ok")
        s = wait(host, lambda s: s["room"] > 0)
        code = s["room"]; print("room", code)
        ok &= code > 0
        ui(guest, open="online")
        wait(guest, lambda s: "JOIN BY CODE" in s["rows"])
        ui(guest, key="down"); guest.run(5); ui(guest, key="down"); guest.run(5)
        ui(guest, key="ok"); guest.run(5)
        ui(guest, text=str(code)); guest.run(5)
        ui(guest, key="enter"); guest.run(5)
        s = wait(guest, lambda s: s["room"] == code)
        print("guest in room:", s["room"] == code, s["rows"])
        ok &= s["room"] == code
        s = wait(host, lambda s: s["members"] >= 2)
        print("host sees", s["members"], "players", s["rows"])
        ok &= s["rows"][:4] == ["WORLD: AREA 51", "LIMIT: FIRST TO 10", "FRAME RATE: GAME", "START"]
        # The host picks EGYPT 2 (two steps right); the guest must see it.
        ui(host, key="right"); host.run(5); ui(host, key="right"); host.run(5)
        s = wait(guest, lambda s: s["rows"][:1] == ["WORLD: EGYPT 2"])
        print("guest sees", s["rows"][:2])
        ok &= s["rows"][:1] == ["WORLD: EGYPT 2"]
        # And FRAME RATE: 60, which both restarted games must agree on (it is
        # part of the match identity, so a mismatch would never connect).
        ui(host, key="down"); host.run(5); ui(host, key="down"); host.run(5)
        ui(host, key="right"); host.run(5)
        s = wait(guest, lambda s: s["rows"][2:3] == ["FRAME RATE: 60"])
        print("guest sees", s["rows"][2:3])
        ok &= s["rows"][2:3] == ["FRAME RATE: 60"]
        ui(host, key="down"); host.run(5)
        ui(host, key="ok")
        alive = [host, guest]
        for _ in range(200):
            for g in list(alive):
                try: g.run(10)
                except Exception: alive.remove(g)   # restarted into the match
            if not alive: break
            time.sleep(0.05)
    time.sleep(40)
    new = exes() - before - mine
    print("restarted games still running after 40 s:", len(new))
    ok &= len(new) == 2
    for pid in new: subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    # Both started in the level (the agreed opening ran before connecting):
    # native pictures exist, and the log says how long the opening took.
    for seat in (0, 1):
        native = list(OUT.glob(f"seat{seat}_*_game.ppm"))
        print(f"seat {seat}: {len(native)} native pictures")
        ok &= len(native) > 0
    for port in (19880, 19881):
        lines = (ROOT / "build" / f"drive_{port}.log").read_text(errors="replace").splitlines()
        print("   ", [l for l in lines if "opening" in l or "missing" in l][-2:])
finally:
    if saved is None: ini.unlink(missing_ok=True)
    else: ini.write_bytes(saved)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

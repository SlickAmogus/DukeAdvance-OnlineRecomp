"""The in-game ONLINE menu end to end: host, join by code, start.

    python tools/re/online_menu_test.py SERVER

Runs two headless copies (DUKE_ONLINE_PREVIEW=1) against SERVER (for example a
local sh_master on 127.0.0.1:27999). The first hosts a room, the second joins
it by its code, the host presses START; both games must then restart into
the link match (new processes, running netplay) and still be running after
40 seconds. Exit code 1 on failure.
"""
import os, pathlib, subprocess, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from drive import Game, ROOT, EXE

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
    with Game(port=19880, env=env) as host, Game(port=19881, env=env) as guest:
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
        ok &= s["rows"][:1] == ["START"]
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
finally:
    if saved is None: ini.unlink(missing_ok=True)
    else: ini.write_bytes(saved)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

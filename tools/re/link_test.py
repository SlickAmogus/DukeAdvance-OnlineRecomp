"""Two linked GBAs on this PC through the framework's link-cable netplay.

    python tools/re/link_test.py [frames] [press ...]

Starts seat 0 and seat 1 of one session over 127.0.0.1 (SDL dummy video and
audio), each with a throwaway save and a scripted pad, and writes each seat's
picture and memory every 60 frames to build/link/. Presses are
"KEY[@seat][+gap]" (KEY = A B START SELECT UP DOWN LEFT RIGHT L R; seat
limits the press to one console; gap = frames to wait after it, default 60).
The default script goes past the language screen and the title.
"""
import os, pathlib, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXE = ROOT / "build" / "DukeAdvanceRecomp.exe"
OUT = ROOT / "build" / "link"
KEYS = dict(A=0x1, B=0x2, SELECT=0x4, START=0x8, RIGHT=0x10, LEFT=0x20, UP=0x40, DOWN=0x80,
            R=0x100, L=0x200)
DEFAULT = ["A+200", "START+200"]

def script(seat, presses, start=1500):
    rows, t = [], start
    for p in presses:
        gap = 60
        if "+" in p: p, g = p.split("+"); gap = int(g)
        only = None
        if "@" in p: p, s = p.split("@"); only = int(s)
        if only is None or only == seat:
            rows.append((t, t + 3, KEYS[p.upper()]))
        t += 4 + gap
    return rows

if __name__ == "__main__":
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    presses = sys.argv[2:] or DEFAULT
    shutil.rmtree(OUT, ignore_errors=True); OUT.mkdir(parents=True, exist_ok=True)
    procs = []
    for seat in (0, 1):
        save = OUT / f"seat{seat}.sav"
        sfile = OUT / f"seat{seat}.script"
        sfile.write_text("".join(f"{a} {b} {m:#x}\n" for a, b, m in script(seat, presses)) or "0 0 0\n")
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   GBARECOMP_DIAGNOSTICS="0", GBARECOMP_NETPLAY_SCRIPT=str(sfile),
                   GBARECOMP_NETPLAY_DUMP=str(OUT / f"seat{seat}"))
        bind, peer = (f"127.0.0.1:{5300 + seat}", f"127.0.0.1:{5301 - seat}")
        argv = [str(EXE), "--save-path", str(save), "--frames", str(frames), "--volume", "0",
                "--netplay-bind", bind, "--netplay-peer", peer,
                "--netplay-seat", str(seat), "--netplay-session", "4242",
                "--netplay-checkpoint", str(OUT / f"seat{seat}.paired")]
        log = open(OUT / f"seat{seat}.log", "w")
        procs.append((subprocess.Popen(argv, cwd=EXE.parent, env=env, stdout=log, stderr=subprocess.STDOUT), log))
    for p, log in procs:
        try: p.wait(timeout=900)
        except subprocess.TimeoutExpired: p.kill()
        log.close()
    for seat in (0, 1):
        dumps = sorted(OUT.glob(f"seat{seat}_*.ppm"), key=lambda q: int(q.stem.split("_")[1]))
        print(f"seat {seat}: exit {procs[seat][0].returncode}, {len(dumps)} pictures")
        print("   ", (OUT / f"seat{seat}.log").read_text(errors="replace").strip().splitlines()[-3:])

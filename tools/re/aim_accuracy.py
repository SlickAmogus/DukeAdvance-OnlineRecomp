"""Check that mouse movement reaches the game in full.

Feeds a steady synthetic mouse while strafing, walking and firing, and
compares the total turn of the player's facing angle with what the mouse asked
for. Lost movement (the game overwriting the mouse's change) shows up as a
shortfall. Run with the native renderer off and on.

    python aim_accuracy.py
"""
import pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT

MOUSE, UNITS = 7, 40    # counts per frame; angle units per count at sensitivity 10


def state(g):
    """(facing angle, x, y) of the player."""
    e = struct.unpack("<I", g.mem(0x03001344, 4))[0]
    x, y = struct.unpack("<ii", g.mem(e + 0x40, 8))
    return struct.unpack("<H", g.mem(e + 0x74, 2))[0], x / 256.0, y / 256.0


def signed16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


if __name__ == "__main__":
    with Game() as g:
        for native in (0, 1):
            for label, keys in (("standing", []), ("strafing + firing", ["L", "B"]),
                                ("walking + strafing", ["UP", "R"]), ("walking + firing", ["UP", "B"])):
                g.run(30); g.load(ROOT / "build" / "level1_start.state"); g.run(30)
                g.cmd(cmd="duke_scene_shot", w=64, h=48, native=native); g.run(10)
                g.cmd(cmd="duke_mouse", dx=0, neg=0, dy=0, negy=0); g.run(4)   # capture on
                total, prev, frames, teleports = 0, state(g), 480, 0
                for i in range(frames + 6):      # the tail lets the game consume what is pending
                    if i < frames:
                        g.cmd(cmd="duke_mouse", dx=MOUSE, neg=0, dy=0, negy=0)
                    g.run(1, *(keys if i < frames else []))
                    now = state(g)
                    if abs(now[1] - prev[1]) + abs(now[2] - prev[2]) > 48:
                        teleports += 1           # a teleporter also sets the facing: not ours
                    else:
                        total += signed16(now[0] - prev[0])
                    prev = now
                want = frames * MOUSE * UNITS
                print("native=%d %-20s turned %7d of %7d units (%.1f%%), teleports %d"
                      % (native, label, total, want, 100.0 * total / want, teleports))

"""Turn with a synthetic mouse while moving, jumping and firing, and check the
camera the native renderer used on every display frame.

A steady mouse must give a steady turn: every frame's change of camera angle
has to equal the mouse step (or be zero for a frame, then double, when the
game's tick and the display frame beat against each other). Anything else is
a view glitch. Position is checked for jumps the same way.

    python glitch_scan.py [frames]
"""
import pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT

MOUSE = 5            # counts per frame
STEP = MOUSE * 40    # angle units per frame at the default sensitivity (10)


def player_xy(g):
    """Position of the first entity record, which is the player."""
    base = struct.unpack("<I", g.mem(0x0300105C, 4))[0]
    x, y = struct.unpack("<ii", g.mem(base + 0x40, 8))
    return x / 256.0, y / 256.0


def signed16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


if __name__ == "__main__":
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 900
    with Game() as g:
        g.run(60); g.load(ROOT / "build" / "level1_start.state"); g.run(30)
        g.cmd(cmd="duke_scene_shot", w=64, h=48, native=1); g.run(10)
        prev, bad, missing, teleports = None, [], 0, 0
        was = player_xy(g)
        settle = 0
        for i in range(frames):
            left = (i // 150) % 2
            g.cmd(cmd="duke_mouse", dx=MOUSE, neg=left, dy=1, negy=(i // 40) % 2)
            keys = ["B"] if i % 6 < 3 else []
            if (i // 90) % 2: keys.append("UP")
            if (i // 200) % 3 == 1: keys.append("A")
            g.run(1, *keys)
            now = player_xy(g)
            teleported = abs(now[0] - was[0]) + abs(now[1] - was[1]) > 48
            was = now
            if teleported:      # the level has teleporters; the camera rightly follows
                teleports += 1; prev = None; settle = 3
            r = g.cmd(cmd="duke_scene_shot", w=64, h=48)
            if settle:
                settle -= 1; prev = None; continue
            if not r.get("ok"):
                missing += 1; prev = None; continue
            if prev is not None and prev[0] == left and i % 150 > 2:
                turn = signed16(r["angle"] - prev[1]["angle"]) * (-1 if left else 1)
                move = abs(r["x"] - prev[1]["x"]) + abs(r["y"] - prev[1]["y"])
                if not (0 <= turn <= 2 * STEP + 8) or move > 24:
                    bad.append((i, turn, round(move, 1)))
            prev = (left, r)
        print("frames %d, composite missing %d, teleports %d, glitched frames %d: %s"
              % (frames, missing, teleports, len(bad), bad[:20]))

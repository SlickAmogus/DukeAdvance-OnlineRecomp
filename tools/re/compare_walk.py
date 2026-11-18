"""Walk through the loaded level and put the game's own picture next to the
native renderer's at each stop. Output goes to the directory given.

    python compare_walk.py <out_dir>
"""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT
from PIL import Image

STEPS = [(60, "UP"), (25, "RIGHT"), (80, "UP"), (40, "LEFT"), (90, "UP"), (30, "LEFT"),
         (90, "UP"), (20, "RIGHT"), (120, "UP"), (64, "RIGHT"), (100, "UP"), (40, "RIGHT"),
         (120, "UP"), (30, "LEFT"), (100, "UP"), (64, "LEFT"), (100, "UP")]


def pair(g):
    g.cmd(cmd="duke_scene_shot", w=240, h=160, native=0)     # switches native off
    g.run(8)
    a = ROOT / "build" / "cmp_a.png"
    g.shot(a)
    g.cmd(cmd="duke_scene_shot", w=240, h=160, native=1)     # back on (too early to capture)
    g.run(8)
    r = g.cmd(cmd="duke_scene_shot", w=240, h=160, native=1)
    left = Image.open(a).convert("RGB")
    right = Image.open(ROOT / "build" / "scene_shot.ppm") if r.get("ok") else Image.new("RGB", (240, 160), (255, 0, 255))
    return left, right


if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    state = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "build" / "level1_start.state")
    with Game() as g:
        g.run(60); g.load(state); g.run(30)
        pairs = [pair(g)]
        for n, key in STEPS:
            g.run(n, key)
            pairs.append(pair(g))
        per = 6
        for s in range(0, len(pairs), per):
            chunk = pairs[s:s + per]
            sheet = Image.new("RGB", (2 * 484, len(chunk) * 324), (40, 40, 40))
            for i, (a, b) in enumerate(chunk):
                sheet.paste(a.resize((480, 320), Image.NEAREST), (0, i * 324))
                sheet.paste(b.resize((480, 320), Image.NEAREST), (484, i * 324))
            sheet.save(out / f"walk_{s // per}.png")
            print(out / f"walk_{s // per}.png")

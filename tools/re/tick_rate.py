"""Measure how often the game finishes a frame (page flips per second of
emulated time) with the native renderer off and on."""
import pathlib, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT


def flips(g, frames=360):
    n, last = 0, None
    for i in range(frames):
        g.run(1, "UP" if (i // 60) % 2 == 0 else "RIGHT")
        v = int(g.cmd(cmd="read_io", addr=0x04000000, len=2)["data"][:2], 16) & 0x10
        if last is not None and v != last:
            n += 1
        last = v
    return n * 60.0 / frames


if __name__ == "__main__":
    with Game() as g:
        g.run(60); g.load(ROOT / "build" / "level1_start.state"); g.run(30)
        for native in (0, 1):
            g.cmd(cmd="duke_scene_shot", w=240, h=160, native=native)
            g.run(10)
            t = time.time()
            rate = flips(g)
            print("native=%d: %.1f game frames/s (host ran %.0f fps)" % (native, rate, 360 / (time.time() - t)))
            g.load(ROOT / "build" / "level1_start.state"); g.run(30)

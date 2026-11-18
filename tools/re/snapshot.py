"""Capture RAM + video memory + a screenshot of the running game for offline
renderer work. Output is ROM-derived: keep it outside the repository.

    python snapshot.py <out_dir>
"""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT


def region(g, cmd, base, size, step=0x2000):
    out = b""
    for off in range(0, size, step):
        r = g.cmd(cmd=cmd, addr=base + off, len=min(step, size - off))
        out += bytes.fromhex(r["data"])
    return out


def snapshot(g, out, name):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    g.shot(out / f"{name}.png")
    for ext, cmd, base, size in (("ewram", "read_ewram", 0x02000000, 0x40000),
                                 ("iwram", "read_iwram", 0x03000000, 0x8000),
                                 ("pal", "read_pal", 0x05000000, 0x400),
                                 ("vram", "read_vram", 0x06000000, 0x18000),
                                 ("oam", "read_oam", 0x07000000, 0x400)):
        (out / f"{name}.{ext}").write_bytes(region(g, cmd, base, size))


if __name__ == "__main__":
    out = sys.argv[1]
    with Game() as g:
        g.run(60)
        g.load(ROOT / "build" / "level1_start.state")
        g.run(30)
        snapshot(g, out, "l1a")
        g.run(40, "UP")
        g.run(25, "RIGHT")
        g.run(10)
        snapshot(g, out, "l1b")
        g.run(60, "UP")
        g.run(30, "LEFT")
        g.run(10)
        snapshot(g, out, "l1c")

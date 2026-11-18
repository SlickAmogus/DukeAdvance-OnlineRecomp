"""Function seeds for the Ghidra decompile (tools/re/DukeDecompile.java).

Real entry points only: Thumb BL targets in the code region, the pointer-table
functions and IWRAM routines from recomp_scan.toml, crt0, and ARM BL targets
inside the IWRAM image. (The recompiler's dispatch table is not used: it
splits functions at every resume point.)

    python tools/re/make_seeds.py <out seeds.txt>
"""
import pathlib, re, struct, sys
root = pathlib.Path(__file__).resolve().parents[2]
rom = (root.parent / "Duke Nukem Advanced # GBA.GBA").read_bytes()
LO, HI = 0xC0, 0x308D0
IW_ROM, IW_RAM, IW_SZ = 0x7FA21C, 0x03003150, 0x411C
h = lambda o: struct.unpack_from("<H", rom, o)[0]
seeds = {}
for o in range(LO & ~1, HI - 2, 2):
    a, b = h(o), h(o + 2)
    if (a & 0xF800) == 0xF000 and (b & 0xF800) == 0xF800:
        off = ((a & 0x7FF) << 12) | ((b & 0x7FF) << 1)
        if off & 0x400000: off -= 0x800000
        t = o + 4 + off
        if LO <= t < HI: seeds[0x08000000 + t] = 1
scan = (root / "recomp_scan.toml").read_text(encoding="utf-8")
for a, m in re.findall(r'addr = 0x([0-9A-F]{8})\nmode = "(\w+)"', scan):
    seeds.setdefault(int(a, 16), 1 if m == "thumb" else 0)
seeds[0x080000C8] = 0; seeds[0x08000130] = 0
for a in (0x080001A8, 0x08002848, 0x08010BC0): seeds[a] = 1
for o in range(0, IW_SZ & ~3, 4):
    w = struct.unpack_from("<I", rom, IW_ROM + o)[0]
    if (w & 0x0F000000) == 0x0B000000 and (w >> 28) == 0xE:
        off = w & 0xFFFFFF
        if off & 0x800000: off -= 0x1000000
        t = IW_RAM + o + 8 + off * 4
        if IW_RAM <= t < IW_RAM + IW_SZ: seeds.setdefault(t, 0)
pathlib.Path(sys.argv[1]).write_text("\n".join("%08X %d" % s for s in sorted(seeds.items())))
print(len(seeds), "seeds")

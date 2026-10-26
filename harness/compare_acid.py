#!/usr/bin/env python3
"""Compare a core_test PPM dump with an acid2 reference PNG.
cgb: exact RGB match (raw colour mode). dmg: match by shade rank, since the
emulator's DMG palette is configurable."""
import sys
from PIL import Image

def load_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    return Image.frombytes("RGB", (160, 144), parts[3])

mode, ppm, ref = sys.argv[1:4]
a = load_ppm(ppm)
b = Image.open(ref).convert("RGB")
if mode == "dmg":
    def ranks(img):
        cols = sorted(set(img.getdata()), key=lambda c: -(c[0] * 3 + c[1] * 6 + c[2]))
        lut = {c: i for i, c in enumerate(cols)}
        return [lut[p] for p in img.getdata()], len(cols)
    ra, na = ranks(a)
    rb, nb = ranks(b)
    diff = sum(1 for x, y in zip(ra, rb) if x != y)
else:
    diff = sum(1 for x, y in zip(a.getdata(), b.getdata()) if x != y)
print("%s %s: %d of %d pixels differ" % (mode, ppm, diff, 160 * 144))
sys.exit(0 if diff == 0 else 1)

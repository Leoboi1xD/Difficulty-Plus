#!/usr/bin/env python3
"""Builds logo.png (336x336, the size Geode wants) from the three faces.

Reads art/ rather than resources/ - the finished frames in resources/ have the
difficulty name baked underneath, which would just be unreadable at logo size.

    python tools/make_logo.py

Pure Python - see pnglib.py, no third-party packages needed.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pnglib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 336

# (art file, centre x, centre y, box) - back two first so the front one overlaps.
LAYOUT = [
    ("DP_casual_face.png",   96, 142, 156),
    ("DP_tough_face.png",   240, 142, 156),
    ("DP_extreme_face.png", 168, 200, 188),
]


def main():
    canvas = bytearray(SIZE * SIZE * 4)

    for name, cx, cy, box in LAYOUT:
        path = os.path.join(ROOT, "art", name)
        w, h, buf = pnglib.read(path)
        w, h, buf = pnglib.trim(w, h, buf)

        scale = box / max(w, h)
        nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
        nw, nh, small = pnglib.resize(w, h, buf, nw, nh)

        pnglib.blit(canvas, SIZE, SIZE, small, nw, nh, cx - nw // 2, cy - nh // 2)
        print("placed %-20s %3dx%-3d at (%d, %d)" % (name, nw, nh, cx, cy))

    out = os.path.join(ROOT, "logo.png")
    pnglib.write(out, SIZE, SIZE, canvas)
    print("wrote logo.png (%dx%d)" % (SIZE, SIZE))


if __name__ == "__main__":
    main()

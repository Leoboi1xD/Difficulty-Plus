#!/usr/bin/env python3
"""Bakes the difficulty name under each face, the way GD's own frames do.

A vanilla difficulty frame is not just a face - it is a 120x120 face at the top,
a 13px gap, then the difficulty name in Pusab at y=134, 34px tall, with the frame
as wide as whichever of the two is wider. Face-only art dropped into that slot
renders about 1.4x too large, so we rebuild the whole frame to the same recipe.

Text is drawn from the game's own bigFont atlas (outline already baked into the
glyphs), so the lettering is identical to vanilla rather than an imitation.

    python tools/make_faces.py --resources "<path to a GD Resources folder>"

Reads face-only art from art/ and writes finished frames to resources/.
The font atlas is only read, never copied into the mod.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pnglib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Vanilla frame geometry, measured from GJ_GameSheet03-uhd (identical in 2.2081
# and the 2.1-era sheets). SCALE multiplies all of it.
#
# Keep this at 1. Matching GD's own frame height means applyTo() computes a scale
# factor of ~1.0 and effectively leaves the sprite's scale alone - which matters,
# because the epic / legendary / mythic glows are ANIMATED children. Scaling the
# parent drags them along, and any correction applied to a child gets overwritten
# by the next tick of its own animation. Authoring at native size sidesteps the
# whole problem instead of fighting it.
SCALE = 1
# These reproduce difficulty_04_btn_001 ("Harder") exactly, measured from
# GJ_GameSheet03-uhd. The numbers matter more than they look: GD's frames are
# trimmed and carry spriteOffset 0,-1, so their real canvas is 170 (or 173) tall,
# not the 168 of the visible ink. Since a sprite centres on its canvas, building
# on a bare 168 puts our label ~1.5px higher than every vanilla one - visible as
# misaligned text in the search filter row.
#
# Vanilla text centre sits +67.5 (Harder) to +68.0 (Normal) below the canvas
# centre; this recipe lands on +67.5.
# The canvas is 173 rather than 170 - the size Easy / Normal / Insane use - which
# buys the three extra rows the taller label needs while keeping the text centre
# at +67.5, so the row still aligns.
FACE = 120 * SCALE      # face box, square
FACE_Y = 2 * SCALE      # face top edge, leaving the padding the trim implies
TEXT_TOP = 136 * SCALE  # text top edge
TEXT_H = 37 * SCALE     # text height (vanilla is 34; ours is a little larger)
FRAME_H = 173 * SCALE   # full canvas, matching GD's untrimmed source size

FACES = [
    ("casual",  "Casual"),
    ("tough",   "Tough"),
    ("extreme", "Extreme"),
]


# --- bitmap font -------------------------------------------------------------

class Font:
    def __init__(self, res_dir):
        fnt = os.path.join(res_dir, "bigFont-uhd.fnt")
        png = os.path.join(res_dir, "bigFont-uhd.png")
        for p in (fnt, png):
            if not os.path.exists(p):
                raise SystemExit("could not find %s\n"
                                 "Point --resources at a folder containing "
                                 "bigFont-uhd.fnt and bigFont-uhd.png." % p)

        text = open(fnt).read()
        self.chars = {}
        for line in text.splitlines():
            if line.startswith("char id="):
                d = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
                self.chars[d["id"]] = d
            elif line.startswith("common "):
                d = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
                self.line_height = d["lineHeight"]
        self.kerning = {}
        for first, second, amount in re.findall(
                r"kerning first=(-?\d+)\s+second=(-?\d+)\s+amount=(-?\d+)", text):
            self.kerning[(int(first), int(second))] = int(amount)

        self.w, self.h, self.atlas = pnglib.read(png)

    def render(self, word):
        """Draw `word` at the atlas's native size, trimmed to its ink."""
        pad = 64
        cw = pad * 2 + sum(self.chars[ord(c)]["xadvance"] for c in word)
        ch = self.line_height + pad * 2
        canvas = bytearray(cw * ch * 4)

        pen = pad
        prev = None
        for c in word:
            g = self.chars[ord(c)]
            if prev is not None:
                pen += self.kerning.get((prev, ord(c)), 0)
            gw, gh = g["width"], g["height"]
            glyph = bytearray(gw * gh * 4)
            for r in range(gh):
                s = ((r + g["y"]) * self.w + g["x"]) * 4
                glyph[r * gw * 4:(r + 1) * gw * 4] = self.atlas[s:s + gw * 4]
            pnglib.blit(canvas, cw, ch, glyph, gw, gh,
                        pen + g["xoffset"], pad + g["yoffset"])
            pen += g["xadvance"]
            prev = ord(c)

        return pnglib.trim(cw, ch, canvas)


# --- composition -------------------------------------------------------------

def face_tint(w, h, buf):
    """The face's representative colour, for tinting its label.

    GD colours every difficulty label to match its face - Easy blue, Hard yellow,
    Insane magenta - so a plain white label reads as dimmer and less native than
    its neighbours. Averaging the face's own opaque, non-outline pixels keeps the
    label in step automatically if the artwork is ever swapped.
    """
    r = g = b = n = 0
    for i in range(w * h):
        if buf[i * 4 + 3] < 200:
            continue
        pr, pg, pb = buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]
        if pr + pg + pb < 210:      # skip the black outline
            continue
        r += pr; g += pg; b += pb; n += 1
    if not n:
        return (255, 255, 255)
    return (r // n, g // n, b // n)


def brighten(c, factor=1.30):
    return tuple(min(255, int(v * factor)) for v in c)


def tint(w, h, buf, color):
    """Multiply-blend `color` over the text.

    The glyphs are a near-white fill inside a black outline, so multiplying turns
    the fill into the colour and leaves the outline black - exactly how GD's own
    coloured labels are built.
    """
    for i in range(w * h):
        if buf[i * 4 + 3] == 0:
            continue
        for k in range(3):
            buf[i * 4 + k] = buf[i * 4 + k] * color[k] // 255
    return buf


def fit(w, h, buf, box_w, box_h):
    scale = min(box_w / w, box_h / h)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    return pnglib.resize(w, h, buf, nw, nh)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--resources", required=True,
                    help="a Geometry Dash Resources folder (for bigFont-uhd.*)")
    ap.add_argument("--check", metavar="WORD",
                    help="render just this word to tmp_<word>.png and stop, "
                         "for comparing against a vanilla frame")
    args = ap.parse_args()

    font = Font(args.resources)

    if args.check:
        w, h, buf = font.render(args.check)
        out = os.path.join(ROOT, "tmp_%s.png" % args.check.lower())
        pnglib.write(out, w, h, buf)
        print("rendered %r -> %s (%dx%d)" % (args.check, out, w, h))
        return

    for name, word in FACES:
        src = os.path.join(ROOT, "art", "DP_%s_face.png" % name)
        fw, fh, face = pnglib.read(src)
        fw, fh, face = pnglib.trim(fw, fh, face)
        fw, fh, face = fit(fw, fh, face, FACE, FACE)

        tw, th, text = font.render(word)
        tw, th, text = fit(tw, th, text, 10 ** 6, TEXT_H)
        colour = brighten(face_tint(fw, fh, face))
        text = tint(tw, th, text, colour)

        frame_w = max(FACE, tw)
        canvas = bytearray(frame_w * FRAME_H * 4)
        pnglib.blit(canvas, frame_w, FRAME_H, face, fw, fh,
                    (frame_w - fw) // 2, FACE_Y + (FACE - fh) // 2)
        pnglib.blit(canvas, frame_w, FRAME_H, text, tw, th,
                    (frame_w - tw) // 2, TEXT_TOP)

        dst = os.path.join(ROOT, "resources", "DP_%s_001.png" % name)
        pnglib.write(dst, frame_w, FRAME_H, canvas)
        print("DP_%-8s face %3dx%-3d + %-8r %3dx%-3d -> frame %3dx%-3d  label rgb%s"
              % (name + "_001", fw, fh, word, tw, th, frame_w, FRAME_H, colour))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Renders the openMouse icon: two crossing pointers, one per seat colour.

Reproducible: run `python tools/make_icon.py` (needs Pillow) to rewrite
res/openmouse.ico. `--preview out.png` also writes a contact sheet of the
real sizes (1:1) and magnified copies, for judging the drawing.
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
YELLOW = (0xFF, 0xDD, 0x00, 255)
ORANGE = (0xE8, 0x6A, 0x33, 255)
TILE = (0x1B, 0x20, 0x2B, 255)
EDGE = (0x8A, 0x93, 0xA6, 255)
INK = (0x10, 0x12, 0x18, 255)

# Classic pointer, tip at the origin, 1.0 = height of the shaft.
ARROW = [(0, 0), (0, 1.0), (0.26, 0.76), (0.43, 1.12), (0.62, 1.03),
         (0.45, 0.68), (0.79, 0.68)]


def place(poly, tip, scale, angle_deg, mirror):
    a = math.radians(angle_deg)
    ca, sa = math.cos(a), math.sin(a)
    out = []
    for x, y in poly:
        x = -x if mirror else x
        out.append((tip[0] + scale * (x * ca - y * sa),
                    tip[1] + scale * (x * sa + y * ca)))
    return out


def render(size):
    n = 8  # supersampling
    S = size * n
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = S * 0.22
    edge = max(1, round(S * 0.035)) if size >= 24 else max(1, round(S * 0.05))
    d.rounded_rectangle([0, 0, S - 1, S - 1], r, fill=EDGE)
    d.rounded_rectangle([edge, edge, S - 1 - edge, S - 1 - edge], r - edge, fill=TILE)

    scale = S * 0.66
    ink = max(2, round(S * (0.045 if size >= 32 else 0.06)))
    # Each pointer starts in an upper corner and leans towards the middle, so
    # the two shafts cross. Orange is behind, yellow in front.
    arrows = [
        (ORANGE, place(ARROW, (S * 0.84, S * 0.12), scale, -8, True)),
        (YELLOW, place(ARROW, (S * 0.16, S * 0.12), scale, 8, False)),
    ]
    for colour, poly in arrows:
        d.line(poly + [poly[0]], fill=INK, width=ink * 2, joint="curve")
        d.polygon(poly, fill=colour)
    return img.resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "res", "openmouse.ico"))
    ap.add_argument("--preview")
    args = ap.parse_args()

    images = {s: render(s) for s in SIZES}
    if args.preview:
        sheet = Image.new("RGBA", (760, 420), (255, 255, 255, 255))
        for i, bg in enumerate(((32, 32, 32, 255), (243, 243, 243, 255))):
            y0 = i * 210
            ImageDraw.Draw(sheet).rectangle([0, y0, 759, y0 + 209], fill=bg)
            x = 10
            for s in (16, 20, 24, 32, 48):
                sheet.alpha_composite(images[s], (x, y0 + 10))
                x += s + 12
            x = 10
            for s in (16, 32, 48):
                big = images[s].resize((s * 4, s * 4), Image.NEAREST)
                sheet.alpha_composite(big, (x, y0 + 70)) if s * 4 + 70 <= 210 else None
                x += s * 4 + 12
            sheet.alpha_composite(images[256].resize((190, 190), Image.LANCZOS), (560, y0 + 10))
        sheet.save(args.preview)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    images[256].save(args.out, format="ICO", sizes=[(s, s) for s in SIZES],
                     append_images=[images[s] for s in SIZES if s != 256])
    print("wrote", os.path.abspath(args.out))


if __name__ == "__main__":
    sys.exit(main())

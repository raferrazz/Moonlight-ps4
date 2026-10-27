#!/usr/bin/env python3
"""Generate deterministic placeholder cover art for the host UI harness.

The covers are original, generated from scratch (vertical gradient + title
initial + decorative geometry); no game artwork is bundled."""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "build-tests/fixture/art"
os.makedirs(OUT, exist_ok=True)

PALETTES = [
    ((18, 38, 71), (94, 60, 153)),   ((16, 63, 92), (13, 129, 141)),
    ((70, 32, 28), (199, 92, 32)),   ((26, 71, 44), (108, 183, 88)),
    ((52, 25, 88), (233, 88, 151)), ((9, 54, 91), (212, 175, 55)),
    ((46, 74, 17), (155, 197, 32)),  ((90, 27, 62), (228, 130, 74)),
    ((30, 30, 34), (120, 124, 136)), ((12, 48, 80), (64, 192, 208)),
    ((84, 44, 12), (240, 196, 32)),  ((14, 40, 70), (120, 90, 220)),
    ((60, 70, 16), (180, 210, 60)),  ((70, 20, 30), (235, 90, 110)),
]

TITLES = ["AM", "BH", "CT", "DC", "EV", "FR", "GD", "HM", "IP", "JS", "KR",
          "LD", "MF", "NG"]

FONT = None
for candidate in (
    "/usr/share/fonts/truetype/english/Carlito-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
):
    if os.path.exists(candidate):
        FONT = ImageFont.truetype(candidate, 210)
        break


def make_cover(idx: int) -> Image.Image:
    top, bottom = PALETTES[idx % len(PALETTES)]
    img = Image.new("RGB", (600, 900))
    d = ImageDraw.Draw(img)
    for y in range(900):
        t = y / 899
        r = int(top[0] + (bottom[0] - top[0]) * t)
        g = int(top[1] + (bottom[1] - top[1]) * t)
        b = int(top[2] + (bottom[2] - top[2]) * t)
        d.line([(0, y), (600, y)], fill=(r, g, b))
    # subtle diagonal light streaks, in theme with the background
    overlay = Image.new("RGBA", (600, 900), (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    for k in range(3):
        off = 140 + k * 90
        od.polygon([(0, off), (600, off - 320), (600, off - 260), (0, off + 60)],
                   fill=(255, 255, 255, 12))
    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
    d = ImageDraw.Draw(img)
    # ring motif
    d.ellipse([170, 250, 430, 510], outline=(255, 255, 255), width=10)
    d.ellipse([220, 300, 380, 460], outline=(255, 255, 255), width=4)
    # title initials
    if FONT:
        text = TITLES[idx % len(TITLES)]
        w = d.textlength(text, font=FONT)
        d.text(((600 - w) / 2, 330), text, font=FONT, fill=(255, 255, 255))
    else:
        d.rectangle([230, 360, 370, 500], fill=(255, 255, 255))
    # bottom band
    d.rectangle([0, 820, 600, 900], fill=(0, 0, 0, 90) if False else (10, 10, 14))
    return img


def main() -> None:
    ids = list(range(1001, 1010)) + list(range(2000, 2014))
    for i, app_id in enumerate(ids):
        make_cover(i).save(os.path.join(OUT, f"{app_id}.png"))
    print(f"wrote {len(ids)} covers to {OUT}")


if __name__ == "__main__":
    main()

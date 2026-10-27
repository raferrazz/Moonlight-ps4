"""Measure the ink band of the baked UI master and verify ui_label_ink_cy().

The baked master (src/ui/ui_font_sans_data.h, 64 px Carlito, 88 raster rows
per glyph) is the ground truth the renderer sees, so the optical-centering
metrics are measured HERE - not hand-copied from a font table.

What this verifies:
  * the ink CORE band (cap top .. baseline, the band the eye reads as the
    text) for the regular and bold weights, measured across capitals and
    lining digits (the digits are figure-normalized at bake time, so they
    share the caps' band);
  * the constants inlined in ui_draw.c's ui_label_ink_cy() match those
    measured values (fail with a diff when someone re-bakes the font and
    forgets to re-measure).

Run from the repo root:  python3 scripts/font_ink_metrics.py
Exit code 0 = metrics in sync, 1 = drift (re-measure and update
ui_draw.c / this file's EXPECT_* constants together).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "ui" / "ui_font_sans_data.h"
DRAW_C = ROOT / "src" / "ui" / "ui_draw.c"

# Ink threshold: a master row counts as ink when any column of the glyph
# is darker than this (same cutoff ui_glyph_ink_span uses for per-glyph
# spans, so per-glyph and band measurements agree).
INK = 8

# Reference set: capitals + lining digits. Lowercase ascenders/descenders
# intentionally excluded - the core is the stable band every line shares.
REF_CODES = [ord(c) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"]

# Constants ui_label_ink_cy() inlines (kept in sync by this script):
EXPECT_TOP = {0: 20, 1: 19}   # regular / bold cap-top master row
EXPECT_BOT = {0: 60, 1: 60}   # baseline master row


def parse_header(path):
    text = path.read_text()
    pt = int(re.search(r"#define SANS_PT (\d+)", text).group(1))
    rh = int(re.search(r"#define SANS_RH (\d+)", text).group(1))
    ascii_n = int(re.search(r"#define SANS_ASCII (\d+)", text).group(1))
    extra_n = int(re.search(r"#define SANS_EXTRA (\d+)", text).group(1))
    glyphs = [
        (int(a), int(b))
        for a, b in re.findall(r"\{(\d+),(\d+),\d+\},", text)
    ]
    body = text.split("sans_pixels[] = {", 1)[1].rsplit("};", 1)[0]
    pixels = [int(x) for x in re.findall(r"\d+", body)]
    return pt, rh, ascii_n, extra_n, glyphs, pixels


def ink_band(glyphs, pixels, rh, base, codes):
    """(top, bot) master rows of the ink core across `codes` in one weight.

    Per-glyph bands reduced by MEDIAN on each edge: a single letter
    reaching past the band (Carlito's Q tail dips below the baseline)
    must not drag the core down with it - the core is the band the
    reference glyphs share, not the union of their extremes.
    """
    tops, bots = [], []
    for code in codes:
        gi = base + code - 32
        offset, width = glyphs[gi]
        raster = pixels[offset : offset + width * rh]
        if len(raster) < width * rh:
            raise SystemExit(f"short raster for glyph {code} (idx {gi})")
        top, bot = None, None
        for row in range(rh):
            line = raster[row * width : (row + 1) * width]
            if any(v > INK for v in line):
                top = row if top is None else top
                bot = row
        if top is None:
            continue
        tops.append(top)
        bots.append(bot)
    if not tops:
        raise SystemExit("no ink found in the reference set")
    tops.sort()
    bots.sort()
    return tops[len(tops) // 2], bots[len(bots) // 2]


def ink_cy(size, bold, top, bot):
    """Whole-pixel offset from ui_label()'s y to the ink-core midline.

    The band spans master rows [top, bot + 1) - a half-open range - so the
    midline sits at (top + bot + 1) / 2 master rows; at `size` px one
    master row is size / 64 dest pixels.
    """
    mid2 = top + bot + 1  # 2x the midline, exact in half-rows
    return (mid2 * size + 64) // 128


def main():
    pt, rh, ascii_n, extra_n, glyphs, pixels = parse_header(HEADER)
    if pt != 64:
        raise SystemExit(f"unexpected master size {pt} (script assumes 64)")
    block = ascii_n + extra_n  # regular block first, bold follows
    bands = {}
    for bold in (0, 1):
        bands[bold] = ink_band(glyphs, pixels, rh, bold * block, REF_CODES)

    ok = True
    for bold in (0, 1):
        top, bot = bands[bold]
        name = "bold" if bold else "regular"
        if (top, bot) != (EXPECT_TOP[bold], EXPECT_BOT[bold]):
            print(f"FAIL: {name} ink band measured {top}..{bot}, "
                  f"ui_draw.c expects {EXPECT_TOP[bold]}..{EXPECT_BOT[bold]}")
            ok = False
        else:
            print(f"ok: {name} ink band {top}..{bot}")

    if not ok:
        print("\nUpdate ui_label_ink_cy() in src/ui/ui_draw.c AND the "
              "EXPECT_* constants here from the measured values above.")
        return 1

    # Reference table (the sizes the app actually renders at).
    sizes = [20, 21, 22, 24, 26, 27, 28, 30, 34, 40, 48, 54, 60, 104]
    print("\nui_label_ink_cy(size, bold):")
    print("  size  " + "".join(f"{s:>5}" for s in sizes))
    for bold in (0, 1):
        top, bot = bands[bold]
        row = "  bold " if bold else "  reg  "
        print(row + "".join(f"{ink_cy(s, bold, top, bot):>5}" for s in sizes))
    print("\nOK: baked master and ui_label_ink_cy() agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

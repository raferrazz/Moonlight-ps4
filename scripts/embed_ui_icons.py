#!/usr/bin/env python3
"""Bake the PS4 UI icon set into a C header for the software rasterizer.

Sources (permissively-licensed icon packs; the source PNGs are inputs to
  this script and are not redistributed in the repository - only the baked
  coverage-mask header is committed):
  - PlayStation button "chips": dark rounded keycap with the white button
    glyph, one PNG per button. Embedded as two coverage masks (chip body
    + white ink) so the renderer keeps the exact shapes at any
    size and can fade the whole chip for disabled hints.
  - monitor.png / controller.png: white-stroke pictograms, embedded as a
    single tintable coverage mask (drawn in the caller's theme color).
  - refs2 set (checkbox pair, close, performance gauge, settings gear,
    info badge): white-stroke or solid-alpha pictograms, tight-cropped
    to their glyph bounds so every draw size is pure glyph geometry.

Output: src/ui/ui_icon_assets.h (coverage bytes, 0..255).
Consumed by ui_draw.c: ui_ps_btn() / ui_icon() blit the masks with the
same area-average sampler used for text, so edges stay crisp at every
render size.

Usage: python3 embed_ui_icons.py ICON_DIR OUTPUT_HEADER [EXTRA_DIRS...]
       ICON_DIR holds T_P4_*.png; the pictograms may live in the same
       directory or be passed as extra search paths.
"""
from pathlib import Path
import sys

import numpy as np
from PIL import Image

CHIP_SRC = 64          # embedded chip canvas (area-averaged from 128)
PAD_SRC = 128          # controller mask canvas
CHIP_L = 34.0          # chip-body luminance ceiling (ink fraction zero point)

# (name, file, kind) - order defines the PSBTN_* id sequence 6..17 in
# ui_draw.h; keep both in sync.
CHIPS = [
    ("circle",    "T_P4_Circle.png"),
    ("cross",     "T_P4_Cross.png"),
    ("triangle",  "T_P4_Triangle.png"),
    ("square",    "T_P4_Square.png"),
    ("options",   "T_P4_Options.png"),
    ("l1",        "T_P4_L1.png"),
    ("r1",        "T_P4_R1.png"),
    ("l2",        "T_P4_L2.png"),
    ("r2",        "T_P4_R2.png"),
    ("l3",        "T_P4_L3.png"),
    ("r3",        "T_P4_R3.png"),
    ("touchpad",  "T_P4_Touch_Pad.png"),
]

MASKS = [
    # (name, file, max_w, max_h) - full-canvas masks (approved look).
    ("monitor", "monitor.png", 184, 168),
    ("pad", "controller.png", 128, 128),
]

# Tight-cropped glyph masks (Material-style pictograms). mode 'white'
# weights alpha by whiteness (white-stroke icons); 'alpha' keeps the
# plain alpha channel (solid-shape icons like the info badge, whose
# glyph is knocked out of a solid disc). All are tinted at draw time.
TIGHT_MASKS = [
    # (name, file, mode, max_w, max_h)
    ("checkbox_off", "check_box_outline.png", "white", 120, 120),
    ("checkbox_on", "select_check_box.png", "white", 120, 120),
    ("close", "close.png", "white", 120, 120),
    ("gauge", "performance.png", "white", 128, 120),
    ("gear", "settings.png", "white", 120, 120),
    ("info", "info.png", "alpha", 128, 128),
]


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float64)


def area_to(arr, w, h):
    """Area-average downscale of a float coverage array to (w, h)."""
    img = Image.fromarray(np.clip(arr * 255.0, 0, 255).astype(np.uint8), "L")
    img = img.resize((w, h), Image.BOX)
    return np.asarray(img, dtype=np.float64) / 255.0


def chip_masks(path):
    """Decompose a chip PNG into (body, ink) coverage, both CHIP_SRC^2."""
    px = load_rgba(path)
    a = px[..., 3] / 255.0
    lum = px[..., :3].max(axis=2)
    ink_frac = np.clip((lum - CHIP_L) / (255.0 - CHIP_L), 0.0, 1.0)
    body = area_to(a, CHIP_SRC, CHIP_SRC)
    ink = area_to(a * ink_frac, CHIP_SRC, CHIP_SRC)
    return body, ink


def tight_bbox(cov, thresh=0.03):
    ys, xs = np.where(cov > thresh)
    if len(xs) == 0:
        return 0, 0, cov.shape[1], cov.shape[0]
    return int(xs.min()), int(ys.min()), int(xs.max() - xs.min() + 1), \
        int(ys.max() - ys.min() + 1)


def pict_mask(path, max_w, max_h):
    """Tintable stroke mask: alpha weighted by whiteness (drops dark fill)."""
    px = load_rgba(path)
    a = px[..., 3] / 255.0
    lum = px[..., :3].max(axis=2) / 255.0
    cov = a * lum
    h, w = cov.shape
    scale = min(max_w / w, max_h / h, 1.0)
    if scale < 1.0:
        cov = area_to(cov, max(1, int(w * scale)), max(1, int(h * scale)))
    return cov


def pict_mask_tight(path, mode, max_w, max_h):
    """Tight-cropped tintable glyph mask.

    Crops to the glyph's tight bbox first (so the emitted geometry is
    pure glyph - the caller sizes the glyph itself), then area-averages
    down to max_w x max_h. 'white' keeps alpha*whiteness (stroke icons);
    'alpha' keeps plain alpha (solid icons whose shape lives entirely in
    the alpha channel, e.g. the info disc with a knocked-out 'i').
    """
    px = load_rgba(path)
    a = px[..., 3] / 255.0
    if mode == "alpha":
        cov = a
    else:
        lum = px[..., :3].max(axis=2) / 255.0
        cov = a * lum
    bx, by, bw, bh = tight_bbox(cov)
    cov = cov[by:by + bh, bx:bx + bw]
    h, w = cov.shape
    scale = min(max_w / w, max_h / h, 1.0)
    if scale < 1.0:
        cov = area_to(cov, max(1, int(w * scale)), max(1, int(h * scale)))
    return cov


def emit_array(lines, name, arr):
    data = np.clip(arr * 255.0 + 0.5, 0, 255).astype(np.uint8).ravel()
    lines.append(f"static const unsigned char {name}[{len(data)}] = {{")
    flat = data.tolist()
    for i in range(0, len(flat), 128):
        lines.append(",".join(map(str, flat[i:i + 128])) + ",")
    lines.append("};")


def main():
    icon_dir = Path(sys.argv[1])
    out = Path(sys.argv[2])
    extra = [Path(p) for p in sys.argv[3:]]  # extra search paths for masks

    def find(name):
        for base in [icon_dir] + extra:
            p = base / name
            if p.exists():
                return p
        sys.exit(f"missing icon asset: {name}")

    lines = [
        "/* Generated by scripts/embed_ui_icons.py - DO NOT EDIT BY HAND.",
        " * PlayStation button chips (dark keycap + white glyph) and the",
        " * monitor / gamepad pictograms, as 0..255 coverage masks.",
        " * ui_draw.c samples them area-averaged (ui_mask_blit) and tints",
        " * the pictogram masks with the caller's theme color. */",
        "#pragma once",
        "",
        f"#define PS_CHIP_SRC {CHIP_SRC}",
        "",
        "typedef struct {",
        "    const unsigned char *body; /* chip silhouette coverage */",
        "    const unsigned char *ink;  /* white glyph coverage */",
        "    short w, h;                /* canvas size */",
        "    short bx, by, bw, bh;      /* tight chip bbox inside canvas */",
        "} ps_chip_t;",
        "",
    ]

    table = []
    for name, fname in CHIPS:
        body, ink = chip_masks(find(fname))
        bx, by, bw, bh = tight_bbox(body)
        emit_array(lines, f"ps_chip_body_{name}", body)
        emit_array(lines, f"ps_chip_ink_{name}", ink)
        table.append((name, bx, by, bw, bh))
        print(f"chip {name}: bbox=({bx},{by},{bw},{bh})")

    lines.append("")
    lines.append("/* Order matches the PSBTN_* ids in ui_draw.h (6..17). */")
    lines.append("static const ps_chip_t k_ps_chip[] = {")
    for name, bx, by, bw, bh in table:
        lines.append(f"    {{ps_chip_body_{name}, ps_chip_ink_{name}, "
                     f"{CHIP_SRC}, {CHIP_SRC}, {bx}, {by}, {bw}, {bh}}},")
    lines.append("};")
    lines.append("")

    for name, fname, mw, mh in MASKS:
        cov = pict_mask(find(fname), mw, mh)
        emit_array(lines, f"ps_mask_{name}", cov)
        lines.append(f"static const int ps_mask_{name}_w = {cov.shape[1]};")
        lines.append(f"static const int ps_mask_{name}_h = {cov.shape[0]};")
        print(f"mask {name}: {cov.shape[1]}x{cov.shape[0]}")

    lines.append("")
    lines.append("/* Tight-cropped glyph masks (refs2 reference set). The emitted")
    lines.append(" * canvas IS the glyph bbox; draw boxes size the glyph. */")
    for name, fname, mode, mw, mh in TIGHT_MASKS:
        cov = pict_mask_tight(find(fname), mode, mw, mh)
        emit_array(lines, f"ps_mask_{name}", cov)
        lines.append(f"static const int ps_mask_{name}_w = {cov.shape[1]};")
        lines.append(f"static const int ps_mask_{name}_h = {cov.shape[0]};")
        print(f"tight mask {name} ({mode}): {cov.shape[1]}x{cov.shape[0]}")

    lines.append("")
    out.write_text("\n".join(lines), encoding="ascii", newline="\n")
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()

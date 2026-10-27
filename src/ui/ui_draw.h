// Software drawing onto a BGRA framebuffer (uint32 0xAARRGGBB little-endian).
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t *fb;
    int pitch; /* bytes per row */
    int w, h;  /* pixels */
} ui_surface_t;

void ui_clear(ui_surface_t *s, uint32_t argb);
void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb);
/* Menu text using proportional sans-serif; scale maps to pixel size. */
void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str);
/* Original lightweight font for per-frame streaming telemetry. */
void ui_text_bitmap(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str);
void ui_text_fit(ui_surface_t *s, int x, int y, int scale, uint32_t argb,
                 const char *str, int max_width);
int ui_text_w(int scale, const char *str);
/* Antialiased proportional sans-serif labels; size is the font pixel size. */
void ui_label(ui_surface_t *s, int x, int y, int size, int bold, uint32_t color, const char *text);
int ui_label_w(int size, int bold, const char *text);
void ui_label_fit(ui_surface_t *s, int x, int y, int size, int bold, uint32_t color, const char *text, int width);

/* --- optical alignment metrics ------------------------------------------
 * ui_label()'s y is the TOP of the glyph row box (1.375 x size tall,
 * ascender headroom above, descender room below). The ink CORE - cap
 * top .. baseline - is the band the eye reads as "the text"; every icon +
 * text pair in the app centers the icon (chip, pictogram, status dot) on
 * that band's midline so the pair reads as one optically centered unit,
 * and descenders are allowed to hang below it, like the console.
 * ui_label_ink_cy() returns the midline offset in whole pixels, measured
 * on the baked master (scripts/font_ink_metrics.py verifies the band;
 * bold ink reaches one master row higher, so the bold midline rides one
 * half-row up). Supersedes the fixed 5/8 core metric. */
int ui_label_ink_cy(int size, int bold);
/* Vertical ink span of one baked glyph in MASTER rows (0..SANS_RH).
 * Returns 0 when the glyph has no ink (space). Used to center single
 * glyphs (OSK keycaps) on their own ink box rather than the row box. */
int ui_glyph_ink_span(int bold, unsigned code, int *top, int *bot);
void ui_round_rect(ui_surface_t *s, int x, int y, int w, int h, int radius, uint32_t color);
/* translucent rounded panel (blends over streamed game frames). */
void ui_round_rect_alpha(ui_surface_t *s, int x, int y, int w, int h,
                         int radius, uint32_t color, int opacity);
/* anti-aliased progress ring; angles 0..1000 permille clockwise
 * from the top (full ring when 0..1000). Blend, no hard edges. */
void ui_ring_arc(ui_surface_t *s, int cx, int cy, int radius, int thickness,
                 uint32_t color, int start_permille, int end_permille);
void ui_overlay(ui_surface_t *s, int x, int y, int w, int h, uint32_t color, int opacity);
void ui_focus(ui_surface_t *s, int x, int y, int w, int h, int radius, uint32_t color, int glow);
void ui_icon(ui_surface_t *s, int x, int y, int size, int type, uint32_t color);
void ui_moonlight_logo(ui_surface_t *s, int cx, int cy, int radius, uint32_t color);
void ui_circle(ui_surface_t *s, int cx, int cy, int radius, uint32_t argb);
void ui_image(ui_surface_t *s, int x, int y, int w, int h,
              const uint8_t *rgba, int iw, int ih);
/* Render a scaled STRAIGHT-alpha tile (same sampling/corners as
 * ui_image_round) into a pre-zeroed w*h ARGB buffer for caching. */
void ui_image_render_rgba(uint32_t *out, int w, int h,
                          const uint8_t *rgba, int iw, int ih, int radius);
/* 1:1 blit of such a cached tile (mix_pixel-exact source-over); opaque
 * pixels take a single-write fast path. This is the glide-frame path for
 * the Games row's art raster cache. */
void ui_blit_rgba(ui_surface_t *s, int x, int y, int w, int h,
                  const uint32_t *src, int stride);
void ui_image_round(ui_surface_t *s, int x, int y, int w, int h,
                    const uint8_t *rgba, int iw, int ih, int radius);

/* --- Orbis design-language primitives -------------------------------- */

/* PS4 focus frame: crisp rectangular outline (sharp corners), the reference
 * Settings/power-menu focus treatment. thickness in pixels. */
void ui_frame(ui_surface_t *s, int x, int y, int w, int h,
              uint32_t color, int thickness);

/* PlayStation button ids for ui_ps_btn(): the shipped chip icons (dark
 * keycap + white glyph). Values 6..10 alias the legacy ui_icon() button
 * types so old call sites keep rendering the right button. */
enum {
    PSBTN_CIRCLE = 6,
    PSBTN_CROSS = 7,
    PSBTN_TRIANGLE = 8,
    PSBTN_SQUARE = 9,
    PSBTN_OPTIONS = 10,
    PSBTN_L1 = 11,
    PSBTN_R1 = 12,
    PSBTN_L2 = 13,
    PSBTN_R2 = 14,
    PSBTN_L3 = 15,
    PSBTN_R3 = 16,
    PSBTN_TOUCHPAD = 17,
};

/* The real PlayStation button chip at `h` pixels tall (width follows the
 * keycap aspect). alpha 0..255 fades the whole chip (disabled hints). */
void ui_ps_btn(ui_surface_t *s, int x, int y, int h, int btn, int alpha);
int ui_ps_btn_w(int h, int btn);

/* Tintable coverage-mask blit (the monitor / gamepad pictograms baked
 * by scripts/embed_ui_icons.py): solid `color` through the mask, area-
 * averaged like text so strokes stay crisp at any size. */
void ui_mask_blit(ui_surface_t *s, int x, int y, int w, int h,
                  const unsigned char *mask, int mw, int mh, uint32_t color);

/* Close "X" pictogram (reference asset mask). (x, y) is the glyph's
 * top-left; `size` is its height. */
void ui_icon_close(ui_surface_t *s, int x, int y, int size, uint32_t color);

/* Info badge (reference asset mask): solid disc with a knocked-out "i",
 * tinted `color`, centered on (cx, cy), `size` tall. */
void ui_icon_info(ui_surface_t *s, int cx, int cy, int size, uint32_t color);

/* Anti-aliased rounded-rectangle outline (keycap bezels, focus rings). */
void ui_round_rect_outline_aa(ui_surface_t *s, int x, int y, int w, int h,
                              int radius, float thickness, uint32_t color);

/* PS4-style working spinner: eight dots orbiting (cx, cy) with a fading
 * trail; advance `frame` once per presented frame (one turn ~0.5 s). */
void ui_spinner(ui_surface_t *s, int cx, int cy, int radius, int frame,
                uint32_t color);

/* Tracked all-caps label (extra letter spacing in px) + width helper. */
void ui_label_caps(ui_surface_t *s, int x, int y, int size, uint32_t color,
                   const char *text, int tracking_px);
int ui_label_caps_w(int size, const char *text, int tracking_px);

/* Global label alpha multiplier for overlay fades (0..255; 255 = normal).
 * Applies to ui_label/ui_label_fit drawing until set back to 255. */
void ui_label_set_alpha(int alpha);

/* PS4-style checkbox (square outline, filled + tick when on). */
void ui_check(ui_surface_t *s, int x, int y, int size, int on, uint32_t color);

/* Chevron (dir: 0 left, 1 right) for steppers. */
void ui_chevron(ui_surface_t *s, int cx, int cy, int size, int dir,
                uint32_t color);

/* Full-surface dim for modal scrims. */
void ui_scrim(ui_surface_t *s, uint32_t color, int opacity);

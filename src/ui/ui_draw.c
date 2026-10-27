#include "ui_draw.h"
#include "ui_font.h"

#include <string.h>
#include <math.h>
#include "ui_font_sans_data.h"
#include "ui_icon_assets.h"

/* ==========================================================================
 * Rendering quality pass - crisp text and shapes at UI sizes.
 *
 *  - TEXT: the font is a 64 px Carlito master (ui_font_sans_data.h) and the
 *    sampler below AREA-AVERAGES (box filter) it down to any request size.
 *    The old 48 px master + 4-tap bilinear undersampled badly at the UI's
 *    20-30 px body sizes (every dest pixel blended only 2 adjacent source
 *    texels per axis even at 2.4x downscale), which frayed thin strokes.
 *  - IMAGES: ui_image_round() is now bilinear (the box art used hard
 *    nearest-neighbor - the "pixelated icons") with an anti-aliased
 *    rounded-corner mask and proper source-alpha compositing.
 *  - SHAPES: rounded rects, the focus ring, the PS button glyphs
 *    (cross/circle/triangle/square), the monitor/controller icons and the
 *    play triangle are all anti-aliased now (4x4 supersampled coverage).
 * ========================================================================== */

static uint32_t mix_pixel(uint32_t under, uint32_t over, unsigned a) {
    unsigned inv = 255 - a;
    return 0xff000000u |
        (((((under >> 16) & 255) * inv + ((over >> 16) & 255) * a) / 255) << 16) |
        (((((under >> 8) & 255) * inv + ((over >> 8) & 255) * a) / 255) << 8) |
        (((under & 255) * inv + (over & 255) * a) / 255);
}
static void pixel(ui_surface_t *s, int x, int y, uint32_t color, unsigned alpha) {
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    uint32_t *p = (uint32_t *)(void *)(s->fb + (size_t)y * s->pitch) + x;
    *p = mix_pixel(*p, color, alpha);
}

/* UTF-8 aware code-point decoder for the baked master. Bytes beyond
 * ASCII map to pseudo-codes (0x100 + codepoint) for the extras block
 * baked after the ASCII weights (SANS_ASCII per weight); anything not
 * baked renders as '?'. */
static unsigned next_char(const char **text) {
    const char *p = *text;
    unsigned c = (unsigned char)*p++;
    if (c == 0xC2 && (unsigned char)*p == 0xB7) { /* U+00B7 middle dot */
        p++;
        c = 0x1B7;
    } else if (c >= 128) {
        while (((unsigned char)*p & 0xc0) == 0x80) p++;
        c = '?';
    }
    *text = p;
    return c;
}

/* Glyph index in sans_glyphs[]: each weight block is SANS_ASCII ASCII
 * glyphs followed by SANS_EXTRA extras (same order as EXTRA in
 * scripts/generate_ui_font.py), regular block first, then bold. */
static unsigned sans_extra_pos(unsigned c) {
    switch (c) {
    case 0x1B7: return 0; /* U+00B7 middle dot */
    }
    return 0xFFFFu; /* not baked */
}

static unsigned sans_index(int bold, unsigned c) {
    if (c >= 0x100) {
        unsigned pos = sans_extra_pos(c);
        if (pos == 0xFFFFu)
            c = '?';
        else
            return (bold ? 2u * SANS_ASCII + SANS_EXTRA : SANS_ASCII) + pos;
    }
    return (unsigned)(bold ? SANS_ASCII + SANS_EXTRA : 0) + c - 32;
}

/* --- text ------------------------------------------------------------------
 * Fixed-point (16.16) area-average resampling of the baked master.
 * STEP = source pixels covered by one dest pixel (SANS_PT<<16 / size).
 * For every dest row/column the touched master texels and their exact
 * overlap weights are precomputed once; the inner loop is then a few
 * multiply-adds per pixel. Downscale is a true box filter (crisp AA),
 * upscale degenerates to bilinear automatically. */

#define GLYPH_MAX_DST_W 112 /* ~widest master glyph at size 128 */
#define GLYPH_MAX_DST_H 184 /* SANS_RH rows at size 128 */

typedef struct {
    int t0;          /* first master texel */
    int n;           /* texels touched (<=4) */
    int t[4];
    unsigned w[4];   /* overlap weight 0..65536 */
} span_t;

static void build_span(span_t *sp, long long idx, long long step, int src_extent) {
    long long f0 = idx * step;
    long long f1 = f0 + step;
    int t0 = (int)(f0 >> 16);
    int t1 = (int)((f1 - 1) >> 16);
    if (t1 - t0 > 3)
        t1 = t0 + 3; /* only possible for step > 4 texels (size < 16) */
    sp->t0 = t0;
    sp->n = 0;
    for (int t = t0; t <= t1; t++) {
        long long a = (long long)t << 16;
        long long b = a + 65536;
        if (a < f0) a = f0;
        if (b > f1) b = f1;
        long long w = b - a;
        if (w <= 0)
            continue;
        if (t < 0 || t >= src_extent)
            continue; /* outside the raster = no ink */
        sp->t[sp->n] = t;
        sp->w[sp->n] = (unsigned)w;
        sp->n++;
    }
}

/* Reciprocal of (step*step) in Q40 for the glyph loop (a plain divide
 * per pixel would dominate the hot loop on the Jaguar cores). */
static unsigned long long s_inv_area;

/* Global label alpha multiplier (0..255). Lets overlay layers fade text
 * without touching every glyph loop; 255 in the normal menu path. */
static unsigned s_label_mul = 255;
void ui_label_set_alpha(int alpha) {
    s_label_mul = alpha < 0 ? 0 : (alpha > 255 ? 255 : (unsigned)alpha);
}

static void ui_label_spanned(ui_surface_t *s, int x, int y, int size,
                             int bold, uint32_t color, const char *text,
                             span_t *ysp, int dst_h, long long step) {
    while (*text) {
        unsigned index = sans_index(bold, next_char(&text));
        int gw = sans_glyphs[index].width;
        const unsigned char *mask = sans_pixels + sans_glyphs[index].offset;
        int dst_w = (gw * size + SANS_PT - 1) / SANS_PT;
        if (dst_w > GLYPH_MAX_DST_W)
            dst_w = GLYPH_MAX_DST_W;
        span_t xsp[GLYPH_MAX_DST_W];
        for (int c = 0; c < dst_w; c++)
            build_span(&xsp[c], c, step, gw);
        for (int r = 0; r < dst_h; r++) {
            const span_t *ry = &ysp[r];
            for (int c = 0; c < dst_w; c++) {
                const span_t *rx = &xsp[c];
                long long num = 0; /* <= 255 * step^2, fits easily */
                for (int ti = 0; ti < ry->n; ti++) {
                    const unsigned char *mrow = mask + (size_t)ry->t[ti] * gw;
                    long long wy = ry->w[ti];
                    for (int tj = 0; tj < rx->n; tj++)
                        num += wy * rx->w[tj] * mrow[rx->t[tj]];
                }
                if (num) {
                    unsigned a = (unsigned)((num * s_inv_area) >> 40);
                    a = a * s_label_mul / 255;
                    if (a)
                        pixel(s, x + c, y + r, color, a > 255 ? 255 : a);
                }
            }
        }
        x += (sans_glyphs[index].advance * size + SANS_PT / 2) / SANS_PT;
    }
}

void ui_label(ui_surface_t *s, int x, int y, int size, int bold, uint32_t color, const char *text) {
    if (!text || size < 8 || size > 128)
        return;
    long long step = ((long long)SANS_PT << 16) / size;
    long long area = step * step;
    s_inv_area = ((1ull << 40) + (unsigned long long)area / 2) /
                 (unsigned long long)area;
    int dst_h = (SANS_RH * size + SANS_PT - 1) / SANS_PT;
    if (dst_h > GLYPH_MAX_DST_H)
        dst_h = GLYPH_MAX_DST_H;
    span_t ysp[GLYPH_MAX_DST_H];
    for (int r = 0; r < dst_h; r++)
        build_span(&ysp[r], r, step, SANS_RH);
    ui_label_spanned(s, x, y, size, bold, color, text, ysp, dst_h, step);
}

int ui_label_w(int size, int bold, const char *text) {
    int width = 0;
    if (!text || size < 8)
        return 0;
    while (*text) {
        unsigned c = next_char(&text);
        width += (sans_glyphs[sans_index(bold, c)].advance * size +
                  SANS_PT / 2) / SANS_PT;
    }
    return width;
}

/* See ui_draw.h: the ink core (cap top .. baseline) of the baked Carlito
 * master spans rows 20..60 (regular) and 19..60 (bold) - measured, not
 * assumed: scripts/font_ink_metrics.py re-measures the baked master and
 * fails when a re-bake moves the band. The band is a half-open row range
 * [top, bot + 1), so its midline sits (top + bot + 1) / 2 master rows
 * below ui_label()'s y; at `size` px one master row is size / 64 dest
 * pixels, and rounding to whole pixels keeps the error under half a
 * pixel at every size the UI renders (18..104). */
int ui_label_ink_cy(int size, int bold) {
    if (size < 8)
        size = 8;
    /* 2x the midline (half-row exact): regular band 20..60, bold 19..60. */
    int mid2 = bold ? (19 + 60 + 1) : (20 + 60 + 1);
    return (mid2 * size + 64) / 128;
}

int ui_glyph_ink_span(int bold, unsigned code, int *top, int *bot) {
    if (code >= 128) /* only the ASCII block carries per-glyph metrics */
        return 0;
    unsigned gi = sans_index(bold, code);
    int gw = sans_glyphs[gi].width;
    const unsigned char *mask = sans_pixels + sans_glyphs[gi].offset;
    int t = -1, b = -1;
    for (int r = 0; r < SANS_RH; r++) {
        const unsigned char *row = mask + (size_t)r * gw;
        for (int c = 0; c < gw; c++) {
            if (row[c] > 8) {
                if (t < 0)
                    t = r;
                b = r;
                break;
            }
        }
    }
    if (t < 0)
        return 0;
    if (top)
        *top = t;
    if (bot)
        *bot = b;
    return 1;
}

void ui_label_fit(ui_surface_t *s, int x, int y, int size, int bold, uint32_t color, const char *text, int width) {
    if (!text || width <= 0) return;
    if (ui_label_w(size, bold, text) <= width) { ui_label(s,x,y,size,bold,color,text); return; }
    char shortened[256]; size_t n = strlen(text);
    if (n > sizeof(shortened)-4) n = sizeof(shortened)-4;
    memcpy(shortened,text,n); shortened[n] = 0;
    while (n && ui_label_w(size,bold,shortened) + ui_label_w(size,bold,"...") > width) {
        do { n--; } while (n && ((unsigned char)shortened[n] & 0xc0) == 0x80);
        shortened[n] = 0;
    }
    memcpy(shortened+n,"...",4); ui_label(s,x,y,size,bold,color,shortened);
}

/* --- AA shape helpers ------------------------------------------------------ */

/* Solid anti-aliased line with round caps (used by the PS button glyphs
 * and the play triangle). 4x4 supersampled coverage. */
static void ui_line_aa(ui_surface_t *s, float x0, float y0, float x1, float y1,
                       float thickness, uint32_t color) {
    float vx = x1 - x0, vy = y1 - y0;
    float len = vx * vx + vy * vy;
    if (len <= 0.0001f)
        return;
    float half = thickness * 0.5f + 0.5f;
    float r2 = half * half;
    int minx = (int)(x0 < x1 ? x0 : x1) - (int)half - 2;
    int maxx = (int)(x0 < x1 ? x1 : x0) + (int)half + 3;
    int miny = (int)(y0 < y1 ? y0 : y1) - (int)half - 2;
    int maxy = (int)(y0 < y1 ? y1 : y0) + (int)half + 3;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            /* Distance from pixel subsamples to the segment, integer math in
             * 1/8 pixel units keeps it exact and fast. */
            unsigned cov = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float fx = px + (sx * 2 + 1) * 0.125f;
                    float fy = py + (sy * 2 + 1) * 0.125f;
                    float t = ((fx - x0) * vx + (fy - y0) * vy) / len;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    float dx = fx - (x0 + t * vx);
                    float dy = fy - (y0 + t * vy);
                    if (dx * dx + dy * dy <= r2)
                        cov++;
                }
            }
            if (cov)
                pixel(s, px, py, color, (cov * 255 + 8) / 16);
        }
    }
}

/* Anti-aliased ring (circle outline) — 4x4 supersampled coverage between
 * r-thickness/2 and r+thickness/2. */
static void ui_ring_aa(ui_surface_t *s, int cx, int cy, float r,
                       float thickness, uint32_t color) {
    float ri = r - thickness * 0.5f, ro = r + thickness * 0.5f;
    float ri2 = ri * ri, ro2 = ro * ro;
    int ext = (int)ro + 2;
    for (int py = -ext; py <= ext; py++) {
        for (int px = -ext; px <= ext; px++) {
            unsigned cov = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float fx = (float)(cx + px) + (sx * 2 + 1) * 0.125f;
                    float fy = (float)(cy + py) + (sy * 2 + 1) * 0.125f;
                    float dx = fx - (float)cx, dy = fy - (float)cy;
                    float d2 = dx * dx + dy * dy;
                    if (d2 <= ro2 && d2 >= ri2)
                        cov++;
                }
            }
            if (cov)
                pixel(s, cx + px, cy + py, color, (cov * 255 + 8) / 16);
        }
    }
}

/* Anti-aliased rounded-rect OUTLINE (button keycaps, monitor bezel). */
void ui_round_rect_outline_aa(ui_surface_t *s, int x, int y, int w, int h,
                                     int radius, float thickness, uint32_t color) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius < 1) {
        ui_rect(s, x, y, w, (int)thickness, color);
        ui_rect(s, x, y + h - (int)thickness, w, (int)thickness, color);
        ui_rect(s, x, y, (int)thickness, h, color);
        ui_rect(s, x + w - (int)thickness, y, (int)thickness, h, color);
        return;
    }
    /* Stroke band [radius - thickness, radius]: INSCRIBED in the box,
     * exactly like ui_frame. Centering the band on the radius instead
     * put the arc's midline half a pixel off the straight edges' band,
     * leaving a visible step at all eight junctions; with the inscribed
     * band the arc's ends land on the same rows/cols the straight edges
     * already paint, so the outline closes seamlessly. */
    float ri = (float)radius - thickness, ro = (float)radius;
    float ri2 = ri * ri, ro2 = ro * ro;
    /* Straight edges (AA against nothing needed: axis-aligned). */
    ui_rect(s, x + radius, y, w - 2 * radius, (int)(thickness + 0.5f), color);
    ui_rect(s, x + radius, y + h - (int)(thickness + 0.5f), w - 2 * radius,
            (int)(thickness + 0.5f), color);
    ui_rect(s, x, y + radius, (int)(thickness + 0.5f), h - 2 * radius, color);
    ui_rect(s, x + w - (int)(thickness + 0.5f), y + radius,
            (int)(thickness + 0.5f), h - 2 * radius, color);
    /* Corner arcs. Each arc iterates ONLY its own corner square
     * (radius x radius): a full ring clipped to the box instead paints
     * its diagonal segments INSIDE the outline - stray curved strokes
     * near every corner that read as doubled borders (found by pixel
     * inspection at the OSK's 106 px / r16 focus ring). */
    for (int k = 0; k < 4; k++) {
        int org_x = (k & 1) ? x + w - radius : x;
        int org_y = (k & 2) ? y + h - radius : y;
        int ctr_x = (k & 1) ? x + w - radius : x + radius;
        int ctr_y = (k & 2) ? y + h - radius : y + radius;
        for (int py = org_y; py < org_y + radius; py++) {
            for (int px = org_x; px < org_x + radius; px++) {
                unsigned cov = 0;
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = (float)px + (sx * 2 + 1) * 0.125f;
                        float fy = (float)py + (sy * 2 + 1) * 0.125f;
                        float dx = fx - (float)ctr_x, dy = fy - (float)ctr_y;
                        float d2 = dx * dx + dy * dy;
                        if (d2 <= ro2 && d2 >= ri2)
                            cov++;
                    }
                }
                if (cov)
                    pixel(s, px, py, color, (cov * 255 + 8) / 16);
            }
        }
    }
}

/* --- basic fills ----------------------------------------------------------- */

void ui_overlay(ui_surface_t *s, int x, int y, int w, int h, uint32_t color, int opacity) {
    if (opacity <= 0) return;
    if (opacity > 255) opacity = 255;
    int x0=x<0?0:x, y0=y<0?0:y, x1=x+w<s->w?x+w:s->w, y1=y+h<s->h?y+h:s->h;
    for(int row=y0;row<y1;row++) for(int col=x0;col<x1;col++) pixel(s,col,row,color,(unsigned)opacity);
}

/* Coverage of one pixel (4x4 subsamples) vs a filled circle. */
static unsigned circle_cov(int px, int py, float ccx, float ccy, float r) {
    unsigned cov = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            float fx = (float)px + (sx * 2 + 1) * 0.125f;
            float fy = (float)py + (sy * 2 + 1) * 0.125f;
            float dx = fx - ccx, dy = fy - ccy;
            if (dx * dx + dy * dy <= r * r)
                cov++;
        }
    return cov;
}

/* Rounded rect with 4x4-supersampled anti-aliased corners (the
 * old version cut the corners with hard 1-pixel steps). */
void ui_round_rect(ui_surface_t *s, int x, int y, int w, int h, int radius, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius > w/2) radius = w/2;
    if (radius > h/2) radius = h/2;
    if (radius < 1) { ui_rect(s, x, y, w, h, color); return; }
    for (int row = 0; row < h; row++) {
        /* Corner-band rows are AA'd against this row's two circle
         * centers; interior rows are a plain solid span. */
        int cy;
        if (row < radius)          cy = y + radius;
        else if (row >= h - radius) cy = y + h - radius - 1;
        else { ui_rect(s, x, y + row, w, 1, color); continue; }
        int dy = row < radius ? radius - row
                              : row - (h - radius - 1);
        int extent = radius;
        while (extent * extent + dy * dy > radius * radius)
            extent--;
        int inset = radius - extent; /* pixels clipped on each side */
        if (inset > 0)
            ui_rect(s, x + inset, y + row, w - 2 * inset, 1, color);
        else
            ui_rect(s, x, y + row, w, 1, color);
        if (!inset)
            continue;
        float fr = (float)radius;
        float ccxl = (float)(x + radius), ccxr = (float)(x + w - radius - 1);
        /* AA the two boundary pixels on each side of the solid span. */
        for (int k = 0; k < 2 && k < inset + 1; k++) {
            int pxl = x + inset - 1 - k;
            if (pxl >= x) {
                unsigned cov = circle_cov(pxl, y + row, ccxl, (float)cy, fr);
                if (cov)
                    pixel(s, pxl, y + row, color, (cov * 255 + 8) / 16);
            }
            int pxr = x + w - inset + k;
            if (pxr < x + w) {
                unsigned cov = circle_cov(pxr, y + row, ccxr, (float)cy, fr);
                if (cov)
                    pixel(s, pxr, y + row, color, (cov * 255 + 8) / 16);
            }
        }
    }
}

/* translucent rounded panel (over streamed video the perf overlay
 * must not hide the game). Same geometry as ui_round_rect, but spans use
 * alpha blending and the AA corners scale their coverage by the opacity. */
void ui_round_rect_alpha(ui_surface_t *s, int x, int y, int w, int h,
                         int radius, uint32_t color, int opacity) {
    if (w <= 0 || h <= 0 || opacity <= 0) return;
    if (opacity > 255) opacity = 255;
    if (radius > w/2) radius = w/2;
    if (radius > h/2) radius = h/2;
    if (radius < 1) { ui_overlay(s, x, y, w, h, color, opacity); return; }
    for (int row = 0; row < h; row++) {
        int cy;
        if (row < radius)          cy = y + radius;
        else if (row >= h - radius) cy = y + h - radius - 1;
        else { ui_overlay(s, x, y + row, w, 1, color, opacity); continue; }
        int dy = row < radius ? radius - row
                              : row - (h - radius - 1);
        int extent = radius;
        while (extent * extent + dy * dy > radius * radius)
            extent--;
        int inset = radius - extent;
        if (inset > 0)
            ui_overlay(s, x + inset, y + row, w - 2 * inset, 1, color, opacity);
        else
            ui_overlay(s, x, y + row, w, 1, color, opacity);
        if (!inset)
            continue;
        float fr = (float)radius;
        float ccxl = (float)(x + radius), ccxr = (float)(x + w - radius - 1);
        for (int k = 0; k < 2 && k < inset + 1; k++) {
            int pxl = x + inset - 1 - k;
            if (pxl >= x) {
                unsigned cov = circle_cov(pxl, y + row, ccxl, (float)cy, fr);
                if (cov)
                    pixel(s, pxl, y + row, color,
                          (cov * opacity + 8) / 16);
            }
            int pxr = x + w - inset + k;
            if (pxr < x + w) {
                unsigned cov = circle_cov(pxr, y + row, ccxr, (float)cy, fr);
                if (cov)
                    pixel(s, pxr, y + row, color,
                          (cov * opacity + 8) / 16);
            }
        }
    }
}

/* anti-aliased progress ring (hold-to-confirm feedback for the
 * in-stream combos). Draws an arc from start_permille to end_permille
 * (0..1000, 0 = top, clockwise) with the given thickness, blended over
 * whatever is below (game frames). Full ring when 0..1000. */
void ui_ring_arc(ui_surface_t *s, int cx, int cy, int radius, int thickness,
                 uint32_t color, int start_permille, int end_permille) {
    if (radius <= 0 || thickness <= 0 || end_permille <= start_permille)
        return;
    if (end_permille > 1000)
        end_permille = 1000;
    if (start_permille < 0)
        start_permille = 0;
    if (end_permille - start_permille >= 1000)
        start_permille = 0, end_permille = 1000;

    float r_out = (float)radius + 0.5f;
    float r_in = (float)radius - (float)thickness;
    /* Angles measured clockwise from the top (screen y grows downward):
     * sample angle = atan2(dx, -dy), 0 at 12 o'clock, +x is 3 o'clock. */
    float a0 = (float)start_permille / 1000.0f * 6.2831853f;
    float a1 = (float)end_permille / 1000.0f * 6.2831853f;
    int full = (start_permille == 0 && end_permille == 1000);

    int bbox = radius + 1;
    for (int py = cy - bbox; py <= cy + bbox; py++) {
        if (py < 0 || py >= s->h)
            continue;
        for (int px = cx - bbox; px <= cx + bbox; px++) {
            if (px < 0 || px >= s->w)
                continue;
            unsigned cov = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float fx = (float)px + (sx * 2 + 1) * 0.125f;
                    float fy = (float)py + (sy * 2 + 1) * 0.125f;
                    float dx = fx - (float)cx, dy = fy - (float)cy;
                    float d2 = dx * dx + dy * dy;
                    if (d2 >= r_out * r_out || d2 <= r_in * r_in)
                        continue;
                    if (full) {
                        cov++;
                    } else {
                        float ang = atan2f(dx, -dy);
                        if (ang < 0)
                            ang += 6.2831853f;
                        if (ang >= a0 && ang <= a1)
                            cov++;
                    }
                }
            }
            if (cov)
                pixel(s, px, py, color, (cov * 255 + 8) / 16);
        }
    }
}

void ui_focus(ui_surface_t *s, int x, int y, int w, int h, int radius, uint32_t color, int glow) {
    /* Paint outside the card only, preserving the image and the background.
     * the ring itself is graded (the old hard 255-alpha 3px band was
     * part of the "rough" look); the glow falloff is unchanged. */
    for (int row = -glow - 4; row < h + glow + 4; row++) {
        for (int col = -glow - 4; col < w + glow + 4; col++) {
            int cx = col < radius ? radius : col >= w - radius ? w - radius - 1 : col;
            int cy = row < radius ? radius : row >= h - radius ? h - radius - 1 : row;
            int dx = col - cx, dy = row - cy, d2 = dx * dx + dy * dy;
            if (d2 < radius * radius)
                continue;
            int distance = radius;
            while (distance * distance < d2 && distance < radius + glow + 5)
                distance++;
            int out = distance - radius;
            if (out <= 3) {
                static const int ring[4] = { 255, 225, 160, 90 };
                pixel(s, x + col, y + row, color, (unsigned)ring[out]);
            } else if (glow && out < glow + 4) {
                pixel(s, x + col, y + row, color, (unsigned)((glow + 4 - out) * 65 / glow));
            }
        }
    }
}

/* --- icons (all anti-aliased) --------------------------------------------
 * Every pictogram now blits a real asset mask baked by
 * scripts/embed_ui_icons.py (monitor / gamepad / checkbox pair / close /
 * performance gauge / settings gear / info badge), area-averaged like
 * text so the strokes stay crisp at any size. Cutouts simply never emit
 * ink, so the theme surface underneath shows through cleanly.
 * type 0 monitor  1 gamepad  2 gauge (live perf)  5 gear
 * type 6 circle   7 cross    8 triangle      9 square   (PS button chips) */

/* Monitor, gamepad, checkbox pair, close, gauge, gear and the info badge
 * are tintable coverage masks baked from the shipped icon sets
 * (scripts/embed_ui_icons.py); the button glyphs are the real
 * PlayStation chips (ui_ps_btn). The vector button/monitor/gamepad code
 * that used to live here could not match the system shapes - the function
 * glyphs read generic and the OPTIONS keycap was broken at half the call
 * sites - so the UI now blits the real assets, area-averaged like text. */

/* --- coverage-mask blits (icon assets) ---------------------------------- */

#define MASK_MAX_DST 160 /* widest mask blit at UI sizes (spans on stack) */

/* Area-average sampler over an 8-bit coverage mask window
 * (sx,sy,sw,sh inside a mw x mh mask), drawn solid in `color`.
 * Same fixed-point math as the text path: true box filter when
 * downscaling, bilinear when upscaling, so chip edges stay crisp at
 * every size the UI asks for. `mul` fades the whole blit (0..255). */
static void mask_blit(ui_surface_t *s, int x, int y, int w, int h,
                      const unsigned char *mask, int mw, int mh,
                      int sx, int sy, int sw, int sh,
                      uint32_t color, int mul) {
    if (!mask || mw <= 0 || mh <= 0 || w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    if (w > MASK_MAX_DST) w = MASK_MAX_DST;
    if (h > MASK_MAX_DST) h = MASK_MAX_DST;
    if (mul < 0) mul = 0;
    if (mul > 255) mul = 255;
    long long step_x = ((long long)sw << 16) / w;
    long long step_y = ((long long)sh << 16) / h;
    long long area = step_x * step_y; /* Q32.32, exactly the text path */
    unsigned long long inv = ((1ull << 40) + (unsigned long long)area / 2) /
                             (unsigned long long)area;
    span_t ysp[MASK_MAX_DST], xsp[MASK_MAX_DST];
    for (int r = 0; r < h; r++)
        build_span(&ysp[r], r, step_y, sh);
    for (int c = 0; c < w; c++)
        build_span(&xsp[c], c, step_x, sw);
    for (int r = 0; r < h; r++) {
        const span_t *ry = &ysp[r];
        for (int c = 0; c < w; c++) {
            const span_t *rx = &xsp[c];
            long long num = 0;
            for (int ti = 0; ti < ry->n; ti++) {
                const unsigned char *mrow =
                    mask + (size_t)(sy + ry->t[ti]) * mw + sx;
                long long wy = ry->w[ti];
                for (int tj = 0; tj < rx->n; tj++)
                    num += wy * rx->w[tj] * mrow[rx->t[tj]];
            }
            if (num) {
                unsigned a = (unsigned)((num * inv) >> 40);
                a = a * (unsigned)mul / 255u;
                if (a)
                    pixel(s, x + c, y + r, color, a > 255 ? 255 : a);
            }
        }
    }
}

void ui_mask_blit(ui_surface_t *s, int x, int y, int w, int h,
                  const unsigned char *mask, int mw, int mh, uint32_t color) {
    mask_blit(s, x, y, w, h, mask, mw, mh, 0, 0, mw, mh, color, 255);
}

/* Shared tight-mask blit: scales the glyph's own bbox into a size-high
 * box, aspect-true, anchored at (x, y). */
static void tight_icon(ui_surface_t *s, int x, int y, int size,
                       const unsigned char *mask, int mw, int mh,
                       uint32_t color) {
    int w = (int)((long long)size * mw / mh);
    mask_blit(s, x, y, w, size, mask, mw, mh, 0, 0, mw, mh, color, 255);
}

/* PlayStation button chip: the shipped keycap icon (dark body + white
 * glyph) scaled so the tight chip bbox is exactly `h` pixels tall.
 * alpha fades the whole chip for disabled hints. */
void ui_ps_btn(ui_surface_t *s, int x, int y, int h, int btn, int alpha) {
    if (btn < PSBTN_CIRCLE || btn > PSBTN_TOUCHPAD || h < 6 || h > 256)
        return;
    const ps_chip_t *c = &k_ps_chip[btn - PSBTN_CIRCLE];
    int w = (int)((long long)c->bw * h / c->bh);
    mask_blit(s, x, y, w, h, c->body, c->w, c->h, c->bx, c->by, c->bw,
              c->bh, 0xFF0F0E17u, alpha);
    mask_blit(s, x, y, w, h, c->ink, c->w, c->h, c->bx, c->by, c->bw,
              c->bh, 0xFFFFFEFFu, alpha);
}

int ui_ps_btn_w(int h, int btn) {
    if (btn < PSBTN_CIRCLE || btn > PSBTN_TOUCHPAD || h <= 0)
        return h;
    const ps_chip_t *c = &k_ps_chip[btn - PSBTN_CIRCLE];
    return (int)((long long)c->bw * h / c->bh);
}

/* PS4-style working spinner: eight dots on a fixed orbit, the fading
 * trail rotating through them. `frame` is the presented-frame counter;
 * two frames per step keeps the same ~0.5 s turn as the launch screen. */
void ui_spinner(ui_surface_t *s, int cx, int cy, int radius, int frame,
                uint32_t color) {
    static const int k_dx[8] = {1024, 724, 0, -724, -1024, -724, 0, 724};
    static const int k_dy[8] = {0, 724, 1024, 724, 0, -724, -1024, -724};
    if (radius < 8 || radius > 512)
        return;
    int slot = (frame >> 1) & 7;
    int r = radius / 6 + 2;
    for (int k = 0; k < 8; k++) {
        int pos = (slot + k) & 7;
        unsigned a = 255u - (unsigned)k * 28u;
        int dx = cx + radius * k_dx[pos] / 1024;
        int dy = cy + radius * k_dy[pos] / 1024;
        for (int yy = -r; yy <= r; yy++) {
            for (int xx = -r; xx <= r; xx++) {
                unsigned hits = 0;
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = (float)xx + (sx * 2 + 1) * 0.125f;
                        float fy = (float)yy + (sy * 2 + 1) * 0.125f;
                        if (fx * fx + fy * fy <= (float)r * r)
                            hits++;
                    }
                }
                if (hits)
                    pixel(s, dx + xx, dy + yy, color,
                          (hits * a + 8) / 16);
            }
        }
    }
}

void ui_icon(ui_surface_t *s, int x, int y, int size, int type, uint32_t color) {
    if (size < 4)
        return;
    if (type == 0) { /* monitor pictogram - tinted, true 184:168 aspect */
        int w = (int)((long long)size * ps_mask_monitor_w /
                      ps_mask_monitor_h);
        mask_blit(s, x + (size - w) / 2, y, w, size, ps_mask_monitor,
                  ps_mask_monitor_w, ps_mask_monitor_h, 0, 0,
                  ps_mask_monitor_w, ps_mask_monitor_h, color, 255);
    } else if (type == 1) { /* gamepad pictogram */
        mask_blit(s, x, y, size, size, ps_mask_pad, ps_mask_pad_w,
                  ps_mask_pad_h, 0, 0, ps_mask_pad_w, ps_mask_pad_h,
                  color, 255);
    } else if (type == 2) {
        /* performance gauge (reference asset): aspect-true, centered in
         * the nominal square so it lines up with the other row icons */
        int w = (int)((long long)size * ps_mask_gauge_w /
                      ps_mask_gauge_h);
        mask_blit(s, x + (size - w) / 2, y, w, size, ps_mask_gauge,
                  ps_mask_gauge_w, ps_mask_gauge_h, 0, 0, ps_mask_gauge_w,
                  ps_mask_gauge_h, color, 255);
    } else if (type == 5) { /* settings gear (reference asset) */
        tight_icon(s, x, y, size, ps_mask_gear, ps_mask_gear_w,
                   ps_mask_gear_h, color);
    } else if (type >= PSBTN_CIRCLE && type <= PSBTN_OPTIONS) {
        /* legacy button-glyph call sites (hold ring, PIN screen, quick
         * menu): render the real chip at the size they asked for. */
        ui_ps_btn(s, x, y, size, type, 255);
    }
}

/* Close "X" (reference asset): the leave/dismiss pictogram. `size` is
 * the glyph height; strokes are aspect-true to the shipped icon. */
void ui_icon_close(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    if (size < 4)
        return;
    tight_icon(s, x, y, size, ps_mask_close, ps_mask_close_w,
               ps_mask_close_h, color);
}

/* Info badge (reference asset): solid disc with a knocked-out "i", so
 * whatever surface it sits on shows through the glyph. Tint with the
 * caller's accent color. */
void ui_icon_info(ui_surface_t *s, int cx, int cy, int size, uint32_t color) {
    if (size < 4)
        return;
    tight_icon(s, cx - size / 2, cy - size / 2, size, ps_mask_info,
               ps_mask_info_w, ps_mask_info_h, color);
}

void ui_moonlight_logo(ui_surface_t *s,int cx,int cy,int radius,uint32_t color) {
    if(radius<1||radius>2048)return;
    /* Eight circular segments, matching pkg/sce_sys/icon0.png. Transparent
     * radial gaps retain the theme background; 4x4 coverage smooths edges. */
    int r=radius*8, gap=r/25, diagonal_gap=gap*1414/1000;
    int x0=cx-radius<0?0:cx-radius, y0=cy-radius<0?0:cy-radius;
    int x1=cx+radius<s->w?cx+radius:s->w-1;
    int y1=cy+radius<s->h?cy+radius:s->h-1;
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++) {
        unsigned covered=0;
        for(int sy=0;sy<4;sy++)for(int sx=0;sx<4;sx++) {
            int dx=(x-cx)*8+sx*2-3,dy=(y-cy)*8+sy*2-3;
            int ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
            int d1=dx-dy,d2=dx+dy;
            if(d1<0)d1=-d1;if(d2<0)d2=-d2;
            if(dx*dx+dy*dy<=r*r&&ax>gap&&ay>gap&&
               d1>diagonal_gap&&d2>diagonal_gap)covered++;
        }
        if(covered)pixel(s,x,y,color,(covered*255+8)/16);
    }
}

void ui_circle(ui_surface_t *s, int cx, int cy, int radius, uint32_t argb) {
    if (radius <= 0 || radius > 2048) return;
    for (int dy = -radius; dy <= radius; dy++) {
        int extent = 0;
        while (extent < radius && (extent + 1) * (extent + 1) + dy * dy <= radius * radius)
            extent++;
        ui_rect(s, cx - extent, cy + dy, extent * 2 + 1, 1, argb);
    }
}

void ui_image(ui_surface_t *s, int x, int y, int w, int h,
              const uint8_t *rgba, int iw, int ih) {
    ui_image_round(s,x,y,w,h,rgba,iw,ih,0);
}

/* Render a scaled tile into a pre-allocated w*h ARGB buffer in STRAIGHT
 * alpha (ui_surface pixel format): a = bilinear source alpha x rounded
 * corner coverage, rgb = un-premultiplied color. Same sampling, crop and
 * corner coverage as ui_image_round() - pixel-for-pixel the same values -
 * but with the alpha kept unbaked so the result can be cached and later
 * composited by ui_blit_rgba() exactly like a direct draw would have.
 * (ui_image_round() itself bakes coverage into RGB by compositing over
 * the destination, so its output cannot be cached.) Buffer must be
 * pre-zeroed; untouched pixels stay fully transparent. */
void ui_image_render_rgba(uint32_t *out, int w, int h,
                          const uint8_t *rgba, int iw, int ih, int radius) {
    if (!out || !rgba || w <= 0 || h <= 0 || iw <= 0 || ih <= 0) return;
    int source_w = iw, source_h = ih;
    if ((long long)iw * h > (long long)ih * w)
        source_w = (int)((long long)ih * w / h);
    else
        source_h = (int)((long long)iw * h / w);
    if (source_w < 1) source_w = 1;
    if (source_h < 1) source_h = 1;
    int source_x = (iw - source_w) / 2, source_y = (ih - source_h) / 2;
    int fx_step = (int)(((long long)source_w << 16) / w);
    int fy_step = (int)(((long long)source_h << 16) / h);
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    for (int row = 0; row < h; row++) {
        uint32_t *dst = out + (size_t)row * (size_t)w;
        long long fy = (long long)row * fy_step;
        int sy0 = source_y + (int)(fy >> 16);
        int sy1 = sy0 + 1 < ih ? sy0 + 1 : sy0;
        int wty8 = (int)((fy & 0xffff) >> 8);         /* 0..255 */
        int wby8 = 255 - wty8;
        int mask_row = 0, cy = 0;
        if (radius > 0) {
            if (row < radius) { mask_row = 1; cy = radius; }
            else if (row >= h - radius) { mask_row = 1; cy = h - radius - 1; }
        }
        float ccxl = (float)radius, ccxr = (float)(w - radius - 1);
        for (int col = 0; col < w; col++) {
            int outa = 255;
            if (mask_row) {
                unsigned cov = 16;
                if (col < radius) {
                    cov = circle_cov(col, row, ccxl, (float)cy,
                                     (float)radius);
                } else if (col >= w - radius) {
                    cov = circle_cov(col, row, ccxr, (float)cy,
                                     (float)radius);
                }
                if (!cov)
                    continue;
                outa = (cov * 255 + 8) / 16;
            }
            long long fx = (long long)col * fx_step;
            int sx0 = source_x + (int)(fx >> 16);
            int sx1 = sx0 + 1 < iw ? sx0 + 1 : sx0;
            int wtx8 = (int)((fx & 0xffff) >> 8);     /* 0..255 */
            int wrx8 = 255 - wtx8;
            /* 2x2 bilinear weights in 0..65025 (sum = 65025 = 255*255) */
            int w00 = wrx8 * wby8, w01 = wtx8 * wby8;
            int w10 = wrx8 * wty8, w11 = wtx8 * wty8;
            const uint8_t *p00 = rgba + ((size_t)sy0 * iw + sx0) * 4;
            const uint8_t *p01 = rgba + ((size_t)sy0 * iw + sx1) * 4;
            const uint8_t *p10 = rgba + ((size_t)sy1 * iw + sx0) * 4;
            const uint8_t *p11 = rgba + ((size_t)sy1 * iw + sx1) * 4;
            unsigned a = ((unsigned)p00[3] * w00 + (unsigned)p01[3] * w01 +
                          (unsigned)p10[3] * w10 + (unsigned)p11[3] * w11) / 65025u;
            if (!a)
                continue;
            unsigned long long pr = (unsigned long long)p00[0] * p00[3] * w00 +
                          (unsigned long long)p01[0] * p01[3] * w01 +
                          (unsigned long long)p10[0] * p10[3] * w10 +
                          (unsigned long long)p11[0] * p11[3] * w11;
            unsigned long long pg = (unsigned long long)p00[1] * p00[3] * w00 +
                          (unsigned long long)p01[1] * p01[3] * w01 +
                          (unsigned long long)p10[1] * p10[3] * w10 +
                          (unsigned long long)p11[1] * p11[3] * w11;
            unsigned long long pb = (unsigned long long)p00[2] * p00[3] * w00 +
                          (unsigned long long)p01[2] * p01[3] * w01 +
                          (unsigned long long)p10[2] * p10[3] * w10 +
                          (unsigned long long)p11[2] * p11[3] * w11;
            unsigned long long denom = (unsigned long long)a * 65025u;
            unsigned rr = (unsigned)(pr / denom);
            unsigned gg = (unsigned)(pg / denom);
            unsigned bb = (unsigned)(pb / denom);
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            unsigned alpha = a * (unsigned)outa / 255u;
            if (!alpha)
                continue;
            dst[col] = ((uint32_t)alpha << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }
}

/* 1:1 blit of a pre-rendered STRAIGHT-alpha ARGB tile (the format
 * ui_image_render_rgba produces) with source-over blending that matches
 * mix_pixel() bit-for-bit, so a cached tile is indistinguishable from a
 * direct ui_image_round() draw. The Games hero row's raster cache
 * (ui_art) hands tiles that were scaled ONCE at cache-fill time; a glide
 * frame then costs a memcpy-class blit per tile instead of a full
 * bilinear resample per tile, which is what kept the console row under
 * 60 fps. Fully opaque pixels take the single-write fast path. */
void ui_blit_rgba(ui_surface_t *s, int x, int y, int w, int h,
                  const uint32_t *src, int stride) {
    if (!src || w <= 0 || h <= 0)
        return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->w ? s->w : x + w;
    int y1 = y + h > s->h ? s->h : y + h;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (int r = y0; r < y1; r++) {
        uint32_t *dst = (uint32_t *)(void *)(s->fb + (size_t)r * s->pitch) + x0;
        const uint32_t *sp = src + (size_t)(r - y) * (size_t)stride +
                             (size_t)(x0 - x);
        int n = x1 - x0;
        while (n--) {
            uint32_t p = *sp++;
            unsigned a = p >> 24;
            if (a == 255) {
                *dst++ = p;
            } else if (a) {
                unsigned inv = 255 - a; /* same formula as mix_pixel() */
                uint32_t d = *dst;
                *dst++ = 0xff000000u |
                    (((((d >> 16) & 255) * inv + ((p >> 16) & 255) * a) / 255) << 16) |
                    (((((d >> 8) & 255) * inv + ((p >> 8) & 255) * a) / 255) << 8) |
                    (((d & 255) * inv + (p & 255) * a) / 255);
            } else {
                dst++;
            }
        }
    }
}

/* Bilinear image blit with source-alpha compositing and an AA rounded
 * mask. The old version sampled nearest-neighbor with integer
 * truncation - upscaling box art 1.4-2.4x made the "pixelated icons". */
void ui_image_round(ui_surface_t *s, int x, int y, int w, int h,
                    const uint8_t *rgba, int iw, int ih, int radius) {
    if (!rgba || w <= 0 || h <= 0 || iw <= 0 || ih <= 0) return;
    int source_w = iw, source_h = ih;
    if ((long long)iw * h > (long long)ih * w)
        source_w = (int)((long long)ih * w / h);
    else
        source_h = (int)((long long)iw * h / w);
    if (source_w < 1) source_w = 1;
    if (source_h < 1) source_h = 1;
    int source_x = (iw - source_w) / 2, source_y = (ih - source_h) / 2;
    int fx_step = (int)(((long long)source_w << 16) / w);
    int fy_step = (int)(((long long)source_h << 16) / h);
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    for (int row = 0; row < h; row++) {
        if (y + row < 0 || y + row >= s->h) continue;
        uint32_t *dst = (uint32_t *)(void *)(s->fb + (size_t)(y + row) * s->pitch);
        long long fy = (long long)row * fy_step;
        int sy0 = source_y + (int)(fy >> 16);
        int sy1 = sy0 + 1 < ih ? sy0 + 1 : sy0;
        int wty8 = (int)((fy & 0xffff) >> 8);         /* 0..255 */
        int wby8 = 255 - wty8;
        /* rounded-mask row setup: only corner-band rows are masked */
        int mask_row = 0, cy = 0;
        if (radius > 0) {
            if (row < radius) { mask_row = 1; cy = y + radius; }
            else if (row >= h - radius) { mask_row = 1; cy = y + h - radius - 1; }
        }
        float ccxl = (float)(x + radius), ccxr = (float)(x + w - radius - 1);
        for (int col = 0; col < w; col++) {
            if (x + col < 0 || x + col >= s->w) continue;
            int outa = 255;
            if (mask_row) {
                /* AA coverage vs this row's two corner circles. */
                unsigned cov = 16;
                if (col < radius) {
                    cov = circle_cov(x + col, y + row, ccxl, (float)cy,
                                     (float)radius);
                } else if (col >= w - radius) {
                    cov = circle_cov(x + col, y + row, ccxr, (float)cy,
                                     (float)radius);
                }
                if (!cov)
                    continue;
                outa = (cov * 255 + 8) / 16;
            }
            long long fx = (long long)col * fx_step;
            int sx0 = source_x + (int)(fx >> 16);
            int sx1 = sx0 + 1 < iw ? sx0 + 1 : sx0;
            int wtx8 = (int)((fx & 0xffff) >> 8);     /* 0..255 */
            int wrx8 = 255 - wtx8;
            /* 2x2 bilinear weights in 0..65025 (sum = 65025 = 255*255) */
            int w00 = wrx8 * wby8, w01 = wtx8 * wby8;
            int w10 = wrx8 * wty8, w11 = wtx8 * wty8;
            const uint8_t *p00 = rgba + ((size_t)sy0 * iw + sx0) * 4;
            const uint8_t *p01 = rgba + ((size_t)sy0 * iw + sx1) * 4;
            const uint8_t *p10 = rgba + ((size_t)sy1 * iw + sx0) * 4;
            const uint8_t *p11 = rgba + ((size_t)sy1 * iw + sx1) * 4;
            /* bilinear alpha (non-premultiplied domain is fine for a) */
            unsigned a = ((unsigned)p00[3] * w00 + (unsigned)p01[3] * w01 +
                          (unsigned)p10[3] * w10 + (unsigned)p11[3] * w11) / 65025u;
            if (!a)
                continue;
            /* channels: premultiplied bilinear so transparent texels do
             * not bleed color into the edges, then undo. Terms reach
             * 255*255*65025 > 2^32, so this is 64-bit math. */
            unsigned long long pr = (unsigned long long)p00[0] * p00[3] * w00 +
                          (unsigned long long)p01[0] * p01[3] * w01 +
                          (unsigned long long)p10[0] * p10[3] * w10 +
                          (unsigned long long)p11[0] * p11[3] * w11;
            unsigned long long pg = (unsigned long long)p00[1] * p00[3] * w00 +
                          (unsigned long long)p01[1] * p01[3] * w01 +
                          (unsigned long long)p10[1] * p10[3] * w10 +
                          (unsigned long long)p11[1] * p11[3] * w11;
            unsigned long long pb = (unsigned long long)p00[2] * p00[3] * w00 +
                          (unsigned long long)p01[2] * p01[3] * w01 +
                          (unsigned long long)p10[2] * p10[3] * w10 +
                          (unsigned long long)p11[2] * p11[3] * w11;
            unsigned long long denom = (unsigned long long)a * 65025u;
            unsigned rr = (unsigned)(pr / denom);
            unsigned gg = (unsigned)(pg / denom);
            unsigned bb = (unsigned)(pb / denom);
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            unsigned alpha = a * (unsigned)outa / 255u;
            if (!alpha)
                continue;
            uint32_t over = 0xff000000u | (rr << 16) | (gg << 8) | bb;
            uint32_t *d = &dst[x + col];
            *d = mix_pixel(*d, over, alpha);
        }
    }
}

void ui_clear(ui_surface_t *s, uint32_t argb) {
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)y * (size_t)s->pitch);
        for (int x = 0; x < s->w; x++)
            row[x] = argb;
    }
}

void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)(y + r) * (size_t)s->pitch);
        for (int c = 0; c < w; c++)
            row[x + c] = argb;
    }
}

static void draw_glyph(ui_surface_t *s, int x, int y, int scale, uint32_t argb,
                       const unsigned char *glyph) {
    for (int gy = 0; gy < UI_GLYPH_H; gy++) {
        unsigned bits = glyph[gy];
        if (!bits)
            continue;
        for (int gx = 0; gx < UI_GLYPH_W; gx++) {
            if (!((bits >> gx) & 1u))
                continue;
            ui_rect(s, x + gx * scale, y + gy * scale, scale, scale, argb);
        }
    }
}

void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str) {
    ui_label(s,x,y,scale < 2 ? 18 : scale * 10,scale >= 4,argb,str);
}
void ui_text_bitmap(ui_surface_t *s,int x,int y,int scale,uint32_t argb,const char *str) {
    if(scale<1)scale=1;
    for(const unsigned char *p=(const unsigned char *)str;*p;p++,x+=UI_GLYPH_W*scale) {
        unsigned c=*p;if(c<32||c>126)c='?';
        draw_glyph(s,x,y,scale,argb,ui_font8x8[c-32]);
    }
}

int ui_text_w(int scale, const char *str) {
    return ui_label_w(scale < 2 ? 18 : scale * 10,scale >= 4,str);
}

void ui_text_fit(ui_surface_t *s, int x, int y, int scale, uint32_t argb,
                 const char *str, int max_width) {
    ui_label_fit(s,x,y,scale < 2 ? 18 : scale * 10,scale >= 4,argb,str,max_width);
}

/* --- Orbis design-language primitives ----------------------------------- */

/* PS4 focus frame: crisp 1..3px rectangular outline. The reference Orbis
 * Settings/power menus wrap the focused row in a sharp-cornered white
 * frame with no fill change - pixel-crisp edges are the look, so no AA
 * is applied (matching the console's 2px frames at 1080p). */
void ui_frame(ui_surface_t *s, int x, int y, int w, int h,
              uint32_t color, int thickness) {
    if (thickness < 1)
        thickness = 1;
    if (thickness > w / 2 || thickness > h / 2)
        return;
    ui_rect(s, x, y, w, thickness, color);
    ui_rect(s, x, y + h - thickness, w, thickness, color);
    ui_rect(s, x, y + thickness, thickness, h - 2 * thickness, color);
    ui_rect(s, x + w - thickness, y + thickness, thickness,
            h - 2 * thickness, color);
}

/* PS button glyph inside an outlined circle - the function-row shape:
 * a thin ring with the cross/circle/triangle/square glyph centered in
 * it, the exact affordance PS4 apps use at the bottom of the screen. */
/* Tracked all-caps micro label (verb lines, wordmark): uppercases ASCII
 * inline and adds letter spacing, the Orbis way of marking system
 * chrome instead of using heavier weights. */
static void caps_buf(const char *text, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)text;
         *p && o + 1 < cap; p++) {
        unsigned c = *p;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

void ui_label_caps(ui_surface_t *s, int x, int y, int size, uint32_t color,
                   const char *text, int tracking_px) {
    char buf[128];
    caps_buf(text, buf, sizeof(buf));
    if (tracking_px <= 0) {
        ui_label(s, x, y, size, 0, color, buf);
        return;
    }
    for (const char *p = buf; *p; p++) {
        char one[2] = { *p, 0 };
        ui_label(s, x, y, size, 0, color, one);
        x += ui_label_w(size, 0, one) + tracking_px;
    }
}

int ui_label_caps_w(int size, const char *text, int tracking_px) {
    char buf[128];
    caps_buf(text, buf, sizeof(buf));
    int w = ui_label_w(size, 0, buf);
    int n = (int)strlen(buf);
    if (n > 1)
        w += (n - 1) * (tracking_px > 0 ? tracking_px : 0);
    return w;
}

/* PS4 checkbox: the reference Material pair, blitted as tintable glyph
 * masks (outline box when off, box + check when on). Both masks share
 * the same box geometry, so the two states never shift a pixel; the
 * on-state canvas is a hair wider for Material's check overshoot. */
void ui_check(ui_surface_t *s, int x, int y, int size, int on,
              uint32_t color) {
    if (on)
        tight_icon(s, x, y, size, ps_mask_checkbox_on,
                   ps_mask_checkbox_on_w, ps_mask_checkbox_on_h, color);
    else
        tight_icon(s, x, y, size, ps_mask_checkbox_off,
                   ps_mask_checkbox_off_w, ps_mask_checkbox_off_h, color);
}

/* Chevron arrow for steppers (dir 0 = left, 1 = right). */
void ui_chevron(ui_surface_t *s, int cx, int cy, int size, int dir,
                uint32_t color) {
    int m = size / 3;
    int u = size / 6 + 1;
    if (dir == 0) {
        ui_line_aa(s, (float)(cx + m), (float)(cy - m),
                   (float)(cx - m), (float)(cy), (float)u, color);
        ui_line_aa(s, (float)(cx - m), (float)(cy),
                   (float)(cx + m), (float)(cy + m), (float)u, color);
    } else {
        ui_line_aa(s, (float)(cx - m), (float)(cy - m),
                   (float)(cx + m), (float)(cy), (float)u, color);
        ui_line_aa(s, (float)(cx + m), (float)(cy),
                   (float)(cx - m), (float)(cy + m), (float)u, color);
    }
}

/* Full-surface dim used behind modal dialogs. */
void ui_scrim(ui_surface_t *s, uint32_t color, int opacity) {
    ui_overlay(s, 0, 0, s->w, s->h, color, opacity);
}

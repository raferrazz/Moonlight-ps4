#include "ui_draw.h"
#include "ui_font.h"

#include <string.h>
#include <math.h>
#include "ui_font_sans_data.h"

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

static unsigned next_char(const char **text) {
    unsigned c = (unsigned char)*(*text)++;
    if (c >= 128) { while (((unsigned char)**text & 0xc0) == 0x80) (*text)++; c = '?'; }
    return c >= 32 && c <= 126 ? c : '?';
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

static void ui_label_spanned(ui_surface_t *s, int x, int y, int size,
                             int bold, uint32_t color, const char *text,
                             span_t *ysp, int dst_h, long long step) {
    while (*text) {
        unsigned index = (bold ? 95 : 0) + next_char(&text) - 32;
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
        width += (sans_glyphs[(bold ? 95 : 0) + c - 32].advance * size +
                  SANS_PT / 2) / SANS_PT;
    }
    return width;
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
static void ui_round_rect_outline_aa(ui_surface_t *s, int x, int y, int w, int h,
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
    float half = thickness * 0.5f;
    float ri = (float)radius - half, ro = (float)radius + half;
    float ri2 = ri * ri, ro2 = ro * ro;
    int cxr[4] = { x + radius, x + w - radius, x + radius, x + w - radius };
    int cyr[4] = { y + radius, y + radius, y + h - radius, y + h - radius };
    /* Straight edges (AA against nothing needed: axis-aligned). */
    ui_rect(s, x + radius, y, w - 2 * radius, (int)(thickness + 0.5f), color);
    ui_rect(s, x + radius, y + h - (int)(thickness + 0.5f), w - 2 * radius,
            (int)(thickness + 0.5f), color);
    ui_rect(s, x, y + radius, (int)(thickness + 0.5f), h - 2 * radius, color);
    ui_rect(s, x + w - (int)(thickness + 0.5f), y + radius,
            (int)(thickness + 0.5f), h - 2 * radius, color);
    /* Corner arcs. */
    for (int k = 0; k < 4; k++) {
        int ccx = cxr[k], ccy = cyr[k];
        for (int py = -radius - 1; py <= radius + 1; py++) {
            for (int px = -radius - 1; px <= radius + 1; px++) {
                int ax = ccx + px, ay = ccy + py;
                if (ax >= x + w || ax < x || ay >= y + h || ay < y)
                    continue;
                unsigned cov = 0;
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = (float)ax + (sx * 2 + 1) * 0.125f;
                        float fy = (float)ay + (sy * 2 + 1) * 0.125f;
                        float dx = fx - (float)ccx, dy = fy - (float)ccy;
                        float d2 = dx * dx + dy * dy;
                        if (d2 <= ro2 && d2 >= ri2)
                            cov++;
                    }
                }
                if (cov)
                    pixel(s, ax, ay, color, (cov * 255 + 8) / 16);
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

/* --- icons (all anti-aliased; the sidebar set is drawn from modern flat-icon
 * references - solid Material-style shapes whose cutouts show the live
 * theme gradient) ----------------------------------------------------------
 * type 0 monitor  1 gamepad  2 badge/details  3 play  4 grid  5 gear
 * type 6 circle   7 cross    8 triangle      9 square   (PS button glyphs) */

/* Geometry helpers in a normalized [0,1] icon box (float keeps the shapes
 * legible from 26 px footer glyphs up to the 120 px Desktop tile art). */
static int in_rrect(float px, float py, float x0, float y0,
                    float x1, float y1, float r) {
    if (px < x0 || px > x1 || py < y0 || py > y1) return 0;
    float qx = px < x0 + r ? x0 + r : (px > x1 - r ? x1 - r : px);
    float qy = py < y0 + r ? y0 + r : (py > y1 - r ? y1 - r : py);
    float dx = px - qx, dy = py - qy;
    return dx * dx + dy * dy <= r * r;
}
static int in_circle(float px, float py, float cx, float cy, float r) {
    float dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

/* Composite a per-pixel coverage function: 4x4 supersampled like the rest
 * of the icon quality pass. Cutouts simply never emit ink, so whatever
 * the theme painted underneath (including the Orbis flow gradient) shows
 * through cleanly on any theme. */
typedef unsigned (*icon_cov_fn)(float px, float py);

static void icon_render(ui_surface_t *s, int x, int y, int size,
                        uint32_t color, icon_cov_fn cov) {
    unsigned ink = (color >> 24) & 255;
    if (!ink || size < 4) return;
    for (int row = 0; row < size; row++) {
        if (y + row < 0 || y + row >= s->h) continue;
        for (int col = 0; col < size; col++) {
            unsigned hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = ((float)col + (sx * 2 + 1) * 0.125f) / (float)size;
                    float fy = ((float)row + (sy * 2 + 1) * 0.125f) / (float)size;
                    if (cov(fx, fy)) hits++;
                }
            if (hits)
                pixel(s, x + col, y + row, color, (hits * ink + 8) / 16);
        }
    }
}

/* Monitor: solid bezel with the screen cut out, short neck and a rounded
 * base - the flat "desktop" icon proportions (~9.5% stroke equivalent).
 * the neck reads thin on TV distances; slightly wider than the
 * first cut. */
static unsigned icon_monitor_cov(float px, float py) {
    if (in_rrect(px, py, 0.04f, 0.13f, 0.96f, 0.60f, 0.09f)) {
        if (!in_rrect(px, py, 0.135f, 0.225f, 0.865f, 0.495f, 0.045f))
            return 1; /* bezel frame around the cut-out screen */
        return 0;
    }
    if (in_rrect(px, py, 0.42f, 0.60f, 0.58f, 0.725f, 0.025f)) return 1;
    if (in_rrect(px, py, 0.26f, 0.725f, 0.74f, 0.825f, 0.05f)) return 1;
    return 0;
}

/* Gamepad: dual-grip silhouette (center bar + two grip lobes) with the
 * D-pad cross and the four action buttons cut out. */
static unsigned icon_gamepad_cov(float px, float py) {
    int body = in_rrect(px, py, 0.04f, 0.26f, 0.96f, 0.66f, 0.14f) ||
               in_circle(px, py, 0.175f, 0.565f, 0.175f) ||
               in_circle(px, py, 0.825f, 0.565f, 0.175f);
    if (!body) return 0;
    /* D-pad cross, left */
    if (in_rrect(px, py, 0.235f, 0.455f, 0.385f, 0.525f, 0.02f) ||
        in_rrect(px, py, 0.275f, 0.415f, 0.345f, 0.565f, 0.02f))
        return 0;
    /* Action-button diamond, right */
    if (in_circle(px, py, 0.69f, 0.392f, 0.052f) ||
        in_circle(px, py, 0.788f, 0.49f, 0.052f) ||
        in_circle(px, py, 0.69f, 0.588f, 0.052f) ||
        in_circle(px, py, 0.592f, 0.49f, 0.052f))
        return 0;
    return 1;
}

/* Gear: central disc, eight blocky rounded-flank teeth and a through-hole.
 * Teeth span ~54% of the pitch (the first cut was too fine and
 * read as a flower at 26 px). The tooth test runs in the frame of the
 * nearest tooth axis, so every tooth is one rounded rectangle. */
static unsigned icon_gear_cov(float px, float py) {
    float dx = px - 0.5f, dy = py - 0.5f;
    float r2 = dx * dx + dy * dy;
    if (r2 > 0.47f * 0.47f) return 0;
    if (r2 <= 0.145f * 0.145f) return 0; /* through-hole */
    if (r2 <= 0.30f * 0.30f) return 1;   /* central disc */
    float r = sqrtf(r2);
    float sector = atan2f(dy, dx) * (4.0f / 3.14159265f); /* 45-degree units */
    float axis = floorf(sector + 0.5f);
    float phi = (sector - axis) * 0.78539816f;            /* rad from axis */
    float lx = r * sinf(phi);   /* tangential offset in the tooth frame */
    float ly = r * cosf(phi);   /* radial distance */
    float qx = lx < -0.078f ? -0.078f : (lx > 0.078f ? 0.078f : lx);
    float qy = ly < 0.295f ? 0.295f : (ly > 0.445f ? 0.445f : ly);
    float ex = lx - qx, ey = ly - qy;
    return ex * ex + ey * ey <= 0.022f * 0.022f;
}

static void icon_monitor(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    icon_render(s, x, y, size, color, icon_monitor_cov);
}

static void icon_gamepad(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    icon_render(s, x, y, size, color, icon_gamepad_cov);
}

static void icon_gear(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    icon_render(s, x, y, size, color, icon_gear_cov);
}

static void icon_grid(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    int spacing = size / 5 + 1;
    int r = size / 12 + 1;
    int mid = size / 2;
    for (int rr = -1; rr <= 1; rr++)
        for (int cc = -1; cc <= 1; cc++)
            ui_circle(s, x + mid + cc * spacing, y + mid + rr * spacing, r, color);
}

static void icon_badge(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    /* Ring with four accent ticks (decode-mode / stream details). */
    ui_ring_aa(s, x + size / 2, y + size / 2, (float)size / 3,
               (float)(size / 9 + 1), color);
    int u = size / 12;
    int c = size / 2;
    ui_rect(s, x + c - u / 2, y + u, u + 1, u * 2 + 1, color);
    ui_rect(s, x + c - u / 2, y + size - u * 3, u + 1, u * 2 + 1, color);
    ui_rect(s, x + u, y + c - u / 2, u * 2 + 1, u + 1, color);
    ui_rect(s, x + size - u * 3, y + c - u / 2, u * 2 + 1, u + 1, color);
}

static unsigned icon_play_cov(float px, float py) {
    /* Solid triangle pointing right in the normalized box. */
    float ax = 5.0f / 24.0f, ay = 7.0f / 24.0f;
    float bx = 5.0f / 24.0f, by = 17.0f / 24.0f;
    float cx = 19.0f / 24.0f, cy = 0.5f;
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    return d1 >= 0 && d2 >= 0 && d3 >= 0;
}

static void icon_play(ui_surface_t *s, int x, int y, int size, uint32_t color) {
    icon_render(s, x, y, size, color, icon_play_cov);
}

void ui_icon(ui_surface_t *s, int x, int y, int size, int type, uint32_t color) {
    if (size < 4)
        return;
    if (type == 0) {
        icon_monitor(s, x, y, size, color);
    } else if (type == 1) {
        icon_gamepad(s, x, y, size, color);
    } else if (type == 2) {
        icon_badge(s, x, y, size, color);
    } else if (type == 5) { /* settings gear */
        icon_gear(s, x, y, size, color);
    } else if (type == 3) {
        icon_play(s, x, y, size, color);
    } else if (type == 4) {
        icon_grid(s, x, y, size, color);
    } else if (type == 9) { /* square button (controller glyph) */
        int inset = size / 6;
        ui_round_rect_outline_aa(s, x + inset, y + inset, size - 2 * inset,
                                 size - 2 * inset, (size - 2 * inset) / 5,
                                 (float)(size / 7 + 1), color);
    } else if (type == 6) { /* circle button */
        ui_ring_aa(s, x + size / 2, y + size / 2, (float)size / 3,
                   (float)(size / 7 + 1), color);
    } else if (type == 7) { /* cross button */
        int m = size / 4, u = size / 7 + 1;
        ui_line_aa(s, (float)(x + m), (float)(y + m),
                   (float)(x + size - m), (float)(y + size - m), (float)u, color);
        ui_line_aa(s, (float)(x + size - m), (float)(y + m),
                   (float)(x + m), (float)(y + size - m), (float)u, color);
    } else if (type == 8) { /* triangle button */
        int m = size / 5 + 1, u = size / 8 + 1;
        int cx = x + size / 2;
        int top = y + m, bl = y + size - m, br = y + size - m;
        int lx = x + m, rx = x + size - m;
        ui_line_aa(s, (float)lx, (float)bl, (float)rx, (float)br, (float)u, color);
        ui_line_aa(s, (float)lx, (float)bl, (float)cx, (float)top, (float)u, color);
        ui_line_aa(s, (float)rx, (float)br, (float)cx, (float)top, (float)u, color);
    }
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

#include "ui_theme.h"
#include "ui_art.h"

#include <stdlib.h>
#include <string.h>

/*
 * Themes are palette + background variants over one native Orbis layout
 * (the way real PS4 themes restyle an app without moving anything). The
 * gradient/ribbon painting below runs ONCE per theme and is cached as a
 * full frame; every redraw afterwards is a row memcpy, which keeps the
 * menu's present-on-change loop cheap even during animation windows.
 */
static int s_index;

/* Midnight, Mono, Blue Wave, Daylight, Cinema, Orbis (default).
 * Orbis is retuned to the stock PS4 blue the console shows everywhere:
 * deep #10305F into #041838 with soft diagonal light, white text, and
 * the light-blue system accent. */
static const ui_theme_t themes[UI_THEME_COUNT] = {
    {"Midnight",  0xFF071322, 0xFF10243A, 0xFF16445B, 0xFF42DDF2, 0xFFF0F6FA, 0xFFA2B8CB, 0xFFFFC46A, 0xFF102B47, 0xFFFFFFFF},
    {"Mono",      0xFF101010, 0xFF1C1C1C, 0xFF363636, 0xFFF1F1EE, 0xFFF1F1EE, 0xFF8B8B89, 0xFFF1F1EE, 0xFF171717, 0xFFF1F1EE},
    {"Blue Wave", 0xFF061B50, 0xFF102E68, 0xFF1C509C, 0xFF9AE9FF, 0xFFF5FAFF, 0xFFBDD0EB, 0xFFFFD07C, 0xFF0E3984, 0xFFFFFFFF},
    {"Daylight",  0xFFF3F0EA, 0xFFE6E3DD, 0xFFD8E5F3, 0xFF326CAE, 0xFF202B39, 0xFF526170, 0xFF92530A, 0xFFFAF8F4, 0xFF26415E},
    {"Cinema",    0xFF101B20, 0xFF1B2D32, 0xFF234950, 0xFF4BDBD7, 0xFFEFF6F6, 0xFFA8BEC3, 0xFFFFC878, 0xFF254650, 0xFFFFFFFF},
    {"Orbis",     0xFF10305F, 0xFF0D2450, 0xFF1B3E77, 0xFF4C9DF5, 0xFFFFFFFF, 0xFFA9BEDC, 0xFFFFC46A, 0xFF041838, 0xFFFFFFFF},
};

void theme_background_invalidate(void);

void ui_theme_set(int index) {
    s_index = index >= 0 && index < UI_THEME_COUNT ? index : 0;
    theme_background_invalidate();
}
int ui_theme_index(void) { return s_index; }
const ui_theme_t *ui_theme_current(void) { return &themes[s_index]; }
const char *ui_theme_name(int index) {
    return themes[index >= 0 && index < UI_THEME_COUNT ? index : 0].name;
}

int ui_theme_move_vertical(int selected, int count, int direction) {
    /* One native content row: UP/DOWN step one slot left/right of the
     * horizontal grid, wrapping - identical to LEFT/RIGHT. The old
     * per-theme grids (two/three columns) are gone with the unified
     * Orbis layout, so the special-case mappings went with them. */
    if (count <= 0)
        return 0;
    if (selected < 0 || selected >= count)
        selected = 0;
    return (selected + count + (direction < 0 ? -1 : 1)) % count;
}

static uint32_t blend(uint32_t a, uint32_t b, int amount) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int av = (a >> shift) & 255, bv = (b >> shift) & 255;
        out |= (uint32_t)(av + (bv - av) * amount / 255) << shift;
    }
    return out;
}

/* --- cached background ------------------------------------------------- */

#define BG_W 1920
#define BG_H 1080

static uint32_t *s_bg_cache;      /* BG_W * BG_H pixels */
static int s_bg_cache_theme = -1;

void theme_background_invalidate(void) { s_bg_cache_theme = -1; }

static void bg_paint_gradient(ui_surface_t *s, const ui_theme_t *t) {
    for (int y = 0; y < s->h; y++)
        ui_rect(s, 0, y, s->w, 1, blend(t->bg, t->horizon, y * 255 / s->h));
}

static void bg_paint_blue_wave(ui_surface_t *s, const ui_theme_t *t) {
    /* Layered flowing ribbons, rather than a straight decorative rule. */
    for (int x = 0; x < s->w; x++) {
        int u = x * 1000 / s->w;
        int y = 70 + u * 3 / 4 - u * u / 1000 + u * u / 1000 * u / 5000;
        for (int layer = 38; layer >= 0; layer--)
            ui_rect(s, x, y + layer, 1, 1, blend(t->bg, 0xff3196ef, 12 + (38 - layer) * 2));
        ui_rect(s, x, y, 1, 2, 0xff247fea);
        int lower = s->h - 150 + u / 5 - u * u / 2200;
        for (int layer = 20; layer >= 0; layer--)
            ui_rect(s, x, lower + layer, 1, 1, blend(t->horizon, 0xff1b6ab8, 25 - layer));
    }
}

static void bg_paint_orbis(ui_surface_t *s, const ui_theme_t *t) {
    /* The stock-PS4 feel: two broad diagonal light bands flowing across
     * the deep blue, plus a soft sheen under the top-right corner -
     * recreated from the reference captures, no system assets. */
    for (int x = 0; x < s->w; x++) {
        int u = x * 1000 / s->w;
        int y1 = 150 + u * 3 / 4 - u * u / 900 + u * u / 1000 * u / 4200;
        for (int layer = 60; layer >= 0; layer--)
            ui_rect(s, x, y1 + layer, 1, 1,
                    blend(t->bg, 0xff4a8ed2, 5 + (60 - layer) / 2));
        ui_rect(s, x, y1, 1, 2, blend(t->bg, 0xff63b3ff, 150));
        int y2 = s->h - 320 + u / 3 - u * u / 1400;
        for (int layer = 40; layer >= 0; layer--)
            ui_rect(s, x, y2 + layer, 1, 1,
                    blend(t->horizon, 0xff2a63b8, 8 - layer / 4));
        ui_rect(s, x, y2, 1, 1, blend(t->horizon, 0xff4a8ed2, 60));
    }
    /* Top-right sheen: a wide diagonal brightening toward the corner. */
    for (int y = 0; y < 420; y++) {
        int reach = 1100 - y * 2;
        for (int x = s->w - reach; x < s->w; x += 1) {
            int into = x - (s->w - reach);
            int a = (y * 30 + into * 55) / 3200;
            if (a > 26)
                a = 26;
            if (a > 0)
                ui_rect(s, x, y, 1, 1, blend(t->bg, 0xff8ec2ef, a));
        }
    }
}

static void bg_build_cache(void) {
    const ui_theme_t *t = ui_theme_current();
    if (!s_bg_cache) {
        s_bg_cache = malloc(sizeof(uint32_t) * (size_t)BG_W * BG_H);
        if (!s_bg_cache)
            return;
    }
    ui_surface_t s = {(uint8_t *)s_bg_cache, BG_W * 4, BG_W, BG_H};
    ui_clear(&s, t->bg);
    bg_paint_gradient(&s, t);
    if (s_index == 2)
        bg_paint_blue_wave(&s, t);
    if (s_index == 5)
        bg_paint_orbis(&s, t);
    s_bg_cache_theme = s_index;
}

void ui_theme_background(ui_surface_t *s) {
    if (s_bg_cache_theme != s_index || !s_bg_cache)
        bg_build_cache();
    if (!s_bg_cache) { /* allocation failed: paint directly (slow path) */
        const ui_theme_t *t = ui_theme_current();
        bg_paint_gradient(s, t);
        return;
    }
    for (int y = 0; y < s->h && y < BG_H; y++)
        memcpy(s->fb + (size_t)y * s->pitch,
               (uint8_t *)s_bg_cache + (size_t)y * BG_W * 4,
               (size_t)(s->w < BG_W ? s->w : BG_W) * 4);
}

void ui_theme_splash(ui_surface_t *s) {
    const ui_theme_t *t = ui_theme_current();
    ui_theme_background(s);
    if (s_index == 0 || s_index == 4) {
        ui_landscape(s, 0, 0, s->w, s->h, 0);
        ui_overlay(s, 0, 0, s->w, s->h, t->bg, s_index == 0 ? 70 : 125);
    }
    int cx = s->w / 2, cy = 350;
    ui_moonlight_logo(s, cx, cy, 112, t->text);
    int size = s_index == 1 ? 60 : 54;
    int track = s_index == 1 ? 6 : 5;
    const char *title = "Moonlight";
    ui_label_caps(s, cx - ui_label_caps_w(size, title, track) / 2, 530, size,
                  t->text, title, track);
    if (s_index == 3)
        ui_rect(s, cx - 25, 705, 50, 4, t->accent);
    ui_label(s, cx - ui_label_w(28, 0, "Starting...") / 2, 850, 28, 0,
             t->text, "Starting...");
}

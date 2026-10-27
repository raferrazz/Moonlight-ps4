/* In-stream HUD drawing in the native PS4 system-layer language.
 * See stream_hud.h for the layering contract.
 *
 * The quick menu follows the reference "Menu rápido" captures: a deep
 * translucent navy panel over the stream occupying the left edge, rows
 * with rectangular focus frames, and a content card to the right showing
 * the selected row's control (PS4 checkbox / stepper) - the split-pane
 * the console uses. Perf overlay and hint pill share the panel language.
 * Colors 0xAARRGGBB (BGRA LE); the output plane ignores alpha. */
#include "stream_hud.h"

#include <stdio.h>
#include <string.h>

#define HUD_BG        0xFF0D1B33u
#define HUD_BG_DEEP   0xFF081222u
#define HUD_ACCENT    0xFF4C9DF5u
#define HUD_TEXT      0xFFFFFFFFu
#define HUD_DIM       0xFFA9BEDCu
#define HUD_GOOD      0xFF6FF08Fu
#define HUD_WARN      0xFFFFB03Fu
#define HUD_BAD       0xFFFF4D4Du
#define HUD_GRAPH_BG  0xFF071120u
#define HUD_TRACK     0xFF2A3A50u

#define HUD_PANEL_ALPHA 232
#define HINT_PILL_ALPHA 190
/* The perf overlay floats over gameplay - keep it readable but let the
 * stream breathe through (the quick menu wants more opacity because it
 * carries primary controls). */
#define STATS_PANEL_ALPHA 158
#define STATS_GRAPH_ALPHA 120

/* --- helpers --------------------------------------------------------------- */

static uint32_t hud_blend(uint32_t a, uint32_t b, int amount) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int av = (a >> shift) & 255, bv = (b >> shift) & 255;
        out |= (uint32_t)(av + (bv - av) * amount / 255) << shift;
    }
    return out;
}

static void hud_hairline(ui_surface_t *s, int x, int y, int w) {
    ui_rect(s, x, y, w, 1, 0x66FFFFFFu);
}

static int hud_fn(ui_surface_t *s, int x, int y, const char *label,
                  int btn) {
    const int ch = 32;
    int cw = ui_ps_btn_w(ch, btn);
    ui_ps_btn(s, x, y, ch, btn, 235);
    /* Label rides the chip's optical midline (ui_label_ink_cy), so the
     * pair reads centered - the first cut sat the text 5 px low. */
    ui_label(s, x + cw + 14, y + ch / 2 - ui_label_ink_cy(24, 0), 24, 0,
             HUD_DIM, label);
    return x + cw + 14 + ui_label_w(24, 0, label) + 56;
}

/* Greedy word wrap for the content-card descriptions: fills out[] with
 * lines that fit max_w at `size` px, returns the line count. The card
 * height follows the count, so long copy grows the box instead of
 * escaping it. */
static int hud_wrap_text(const char *text, int size, int max_w,
                         char out[][96], int max_lines) {
    char line[160];
    int n = 0;
    line[0] = 0;
    const char *p = text;
    while (*p && n < max_lines) {
        const char *sp = p;
        while (*sp && *sp != ' ')
            sp++;
        char word[80];
        int len = (int)(sp - p);
        if (len > 79)
            len = 79;
        memcpy(word, p, (size_t)len);
        word[len] = 0;
        char trial[224];
        snprintf(trial, sizeof(trial), "%s%s%s", line,
                 line[0] ? " " : "", word);
        if (!line[0] || ui_label_w(size, 0, trial) <= max_w) {
            snprintf(line, sizeof(line), "%s", trial);
        } else {
            snprintf(out[n++], 96, "%s", line);
            snprintf(line, sizeof(line), "%s", word);
        }
        p = *sp ? sp + 1 : sp;
    }
    if (n < max_lines && line[0])
        snprintf(out[n++], 96, "%s", line);
    return n;
}

/* --- quick menu: left panel + right content card ----------------------------- */

#define QM_PANEL_W 560
#define QM_CARD_W 600
#define QM_ROWS 5

static const char *k_qm_labels[QM_ROWS] = {
    "Touchpad mouse", "Performance overlay", "Bitrate", "Quit stream",
    "Quit app on PC",
};

static void qm_row_icon(ui_surface_t *s, int x, int y, int row, int sel) {
    uint32_t color = sel ? HUD_TEXT : HUD_DIM;
    /* y is the icon column's base (row top + 14); every icon centers on
     * the ROW's midline (y + 24), so chips, pictograms and the bar
     * group all share one optical line with the label. */
    switch (row) {
    case QM_ROW_TOUCHPAD:
        /* The system's touchpad keycap chip - the wide T_P4_Touch_Pad
         * asset (dark keycap + white glyph), same family as the button
         * chips in the panel footer. Chip bbox is 3:2, so it draws
         * 48 x 32 centered on the 40 px icon column. */
        ui_ps_btn(s, x - 4, y + 8, 32, PSBTN_TOUCHPAD,
                  sel ? 255 : 170);
        break;
    case QM_ROW_OVERLAY:
        ui_icon(s, x, y + 4, 40, 2, color); /* gauge: live perf readout */
        break;
    case QM_ROW_BITRATE: {
        /* Signal bars - drawn inline so no new icon type is needed.
         * The group centers on the row midline and on the icon column
         * (it used to hug the column top and sit 7 px high). */
        for (int k = 0; k < 4; k++) {
            int bh = 8 + k * 9;
            ui_rect(s, x + 3 + k * 9, y + 41 - bh, 6, bh, color);
        }
        break;
    }
    case QM_ROW_QUIT:
        /* Leaving the stream = the simple close cross (reference asset). */
        ui_icon_close(s, x + 2, y + 6, 36, color);
        break;
    default:
        /* Closing the app on the host = the monitor pictogram. */
        ui_icon(s, x, y + 4, 40, 0, color);
        break;
    }
}

void stream_hud_quick_menu(ui_surface_t *s, const qm_view_t *m,
                           int reveal_permille) {
    if (reveal_permille < 0)
        reveal_permille = 0;
    if (reveal_permille > 1000)
        reveal_permille = 1000;
    /* Ease-out slide from the left edge. */
    int inv = 1000 - reveal_permille;
    int slide = QM_PANEL_W * inv * inv / (1000 * 1000);
    int px = -slide;
    int card_alpha = reveal_permille < 400
                         ? HUD_PANEL_ALPHA * (reveal_permille - 100) / 300
                         : HUD_PANEL_ALPHA;
    if (card_alpha < 0)
        card_alpha = 0;
    if (card_alpha > HUD_PANEL_ALPHA)
        card_alpha = HUD_PANEL_ALPHA;

    /* Panel: full-height translucent navy over the stream. */
    ui_round_rect_alpha(s, px, 0, QM_PANEL_W, s->h, 0, HUD_BG,
                        HUD_PANEL_ALPHA);
    ui_rect(s, px + QM_PANEL_W - 2, 0, 2, s->h,
            hud_blend(HUD_ACCENT, HUD_BG, 90));

    /* Header. */
    ui_label(s, px + 56, 66, 40, 0, HUD_TEXT, "Stream menu");
    ui_label(s, px + 56, 118, 22, 0, HUD_DIM,
             m->bitrate_changed ? "Bitrate applies on close" : "Moonlight");
    hud_hairline(s, px + 56, 156, QM_PANEL_W - 112);

    /* Rows. */
    int y0 = 212, pitch = 104;
    for (int i = 0; i < QM_ROWS; i++) {
        int y = y0 + i * pitch;
        int sel = i == m->sel;
        if (sel)
            ui_frame(s, px + 32, y - 4, QM_PANEL_W - 64, 84, HUD_ACCENT, 2);
        qm_row_icon(s, px + 56, y + 14, i, sel);
        /* Label core (cap..baseline center) lands on the row midline
         * (y + 38), the same line the icons center on. */
        ui_label(s, px + 116, y + 20, 28, sel, sel ? HUD_TEXT : HUD_DIM,
                 k_qm_labels[i]);
    }

    /* Footer verbs inside the panel. */
    int fx = px + 56, fy = s->h - 74;
    fx = hud_fn(s, fx, fy, "Select", 7);
    fx = hud_fn(s, fx, fy, "Close", 6);

    /* Content card for the selected row (split-pane, like the system
     * quick menu's right side). The card sizes itself from its content:
     * quit descriptions word-wrap to the card width and set its height,
     * so the copy can never run past the frame again. */
    if (card_alpha <= 0)
        return;
    int cx = QM_PANEL_W + 96, cy = 380, cw = QM_CARD_W;
    int ch = 320;
    char lines[3][96];
    int nlines = 0;
    const char *desc = NULL;
    if (m->sel == QM_ROW_QUIT)
        desc = "Back to the Moonlight menu. The game keeps running on "
               "your PC.";
    else if (m->sel == QM_ROW_QUIT_APP)
        desc = "Ends the stream and closes the game on your PC, then "
               "returns to the Games list.";
    if (desc) {
        nlines = hud_wrap_text(desc, 27, cw - 88, lines, 3);
        ch = 124 + nlines * 40 + 44;
        if (ch < 260)
            ch = 260;
    }
    ui_round_rect_alpha(s, cx, cy, cw, ch, 12, HUD_BG_DEEP, card_alpha);
    ui_rect(s, cx, cy, 3, ch, HUD_ACCENT);

    ui_label(s, cx + 44, cy + 44, 30, 1, HUD_TEXT, k_qm_labels[m->sel]);
    hud_hairline(s, cx + 44, cy + 96, cw - 88);
    switch (m->sel) {
    case QM_ROW_TOUCHPAD:
    case QM_ROW_OVERLAY: {
        int on = m->sel == QM_ROW_TOUCHPAD ? m->touchpad : m->overlay;
        ui_check(s, cx + 44, cy + 128, 56, on, HUD_ACCENT);
        ui_label(s, cx + 128, cy + 138, 28, 0, on ? HUD_TEXT : HUD_DIM,
                 on ? "On" : "Off");
        ui_label(s, cx + 44, cy + 224, 22, 0, HUD_DIM,
                 m->sel == QM_ROW_TOUCHPAD
                     ? "Drag the touchpad to move the host cursor"
                     : "Live FPS, timings and network readout");
        break;
    }
    case QM_ROW_BITRATE: {
        char v[32];
        snprintf(v, sizeof(v), "%d", m->bitrate);
        int vw = ui_label_w(54, 1, v);
        int vx = cx + (cw - vw) / 2;
        /* Value dead-center between the arrows (the first cut offset it
         * 40 px left of the card's midline). Arrows ride the value's
         * optical midline, 56 px off each edge. */
        ui_label(s, vx, cy + 112, 54, 1, HUD_TEXT, v);
        ui_chevron(s, vx - 56, cy + 146, 40, 0, HUD_DIM);
        ui_chevron(s, vx + vw + 56, cy + 146, 40, 1, HUD_DIM);
        /* Steppers: fine step on the D-pad, the Settings-row step on the
         * shoulders (kept in sync with QM_BITRATE_STEP / _BIG). */
        const char *note =
            "LEFT / RIGHT steps 1000  \xc2\xb7  L1 / R1 steps 5000";
        ui_label(s, cx + (cw - ui_label_w(22, 0, note)) / 2,
                 cy + 224, 22, 0, HUD_DIM, note);
        break;
    }
    case QM_ROW_QUIT:
    default:
        for (int i = 0; i < nlines; i++)
            ui_label(s, cx + 44, cy + 124 + i * 40, 27, 0, HUD_DIM,
                     lines[i]);
        break;
    }
}

/* --- performance overlay ------------------------------------------------------ */

void stream_hud_stats(ui_surface_t *s, const video_live_stats_t *live) {
    const int px = 28, py = 28, pw = 412;
    const int pad = 26;
    const int graph_w = pw - pad * 2, graph_h = 54;
    /* Row budget: header + divider + 6 value rows + graph (the Dropped
     * row joined the Network readout). */
    const int header_h = 92;
    const int row_h = 46;
    const int total_rows = 6;
    const int ph = pad + header_h + 8 + row_h * total_rows + 14 + graph_h + pad;
    ui_round_rect_alpha(s, px, py, pw, ph, 10, HUD_BG, STATS_PANEL_ALPHA);
    /* System-layer accent edge + tracked caps micro-label. */
    ui_rect(s, px + 12, py + 14, 44, 3, HUD_ACCENT);
    ui_label_caps(s, px + 70, py + 8, 20, HUD_DIM, "Performance", 4);

    /* Header: big FPS reading + stream shape. The unit rides the
     * number's baseline (60/64*size), the same rule the corner metadata
     * pair uses. */
    char v[96];
    snprintf(v, sizeof(v), "%.1f", (double)live->fps);
    ui_label(s, px + pad, py + 46, 48, 1, HUD_TEXT, v);
    int vw = ui_label_w(48, 1, v);
    ui_label(s, px + pad + vw + 12,
             py + 46 + (60 * (48 - 22) + 32) / 64, 22, 0, HUD_DIM, "fps");

    /* Right-corner metadata pair; the resolution shares the caps
     * label's baseline (both are 20-21 px metadata, one visual line). */
    snprintf(v, sizeof(v), "%dx%d", s->w, s->h);
    int hw = ui_label_w(21, 0, v);
    ui_label(s, px + pw - pad - hw, py + 7, 21, 0, HUD_DIM, v);
    snprintf(v, sizeof(v), "%.0f KB/frame", (double)live->kb_per_frame);
    hw = ui_label_w(21, 0, v);
    ui_label(s, px + pw - pad - hw, py + 53, 21, 0, HUD_DIM, v);

    hud_hairline(s, px + pad, py + header_h + 8, pw - pad * 2);

    int ty = py + header_h + 24;
    const int vl = 22, vv = 24;
    struct { const char *name; char value[64]; uint32_t col; } rows[6];
    int nrows = 0;

    snprintf(rows[nrows].value, sizeof(rows[nrows].value),
             "%.1f + %.1f ms", (double)live->decode_ms,
             (double)live->convert_ms);
    rows[nrows].name = "Decode + convert";
    rows[nrows].col = HUD_TEXT;
    nrows++;

    snprintf(rows[nrows].value, sizeof(rows[nrows].value), "%.1f ms",
             (double)live->present_ms);
    rows[nrows].name = "Present";
    rows[nrows].col = HUD_TEXT;
    nrows++;

    if (live->host_ms > 0.0f) {
        snprintf(rows[nrows].value, sizeof(rows[nrows].value),
                 "recv %.1f ms  host %.1f ms", (double)live->net_recv_ms,
                 (double)live->host_ms);
    } else {
        snprintf(rows[nrows].value, sizeof(rows[nrows].value), "recv %.1f ms",
                 (double)live->net_recv_ms);
    }
    rows[nrows].name = "Network";
    rows[nrows].col = HUD_ACCENT;
    nrows++;

    snprintf(rows[nrows].value, sizeof(rows[nrows].value), "%.1f Mbps",
             (double)live->rx_mbps);
    rows[nrows].name = "Video bitrate";
    rows[nrows].col = HUD_TEXT;
    nrows++;

    /* Dropped frames in the live window, colored by severity. The session
     * fps is known from setup (target_fps), so the thresholds are
     * relative: a 30 fps stream tolerates proportionally more frame time
     * than a 60 fps one, and the verdict says the same thing. */
    {
        unsigned dropped = live->dropped_window;
        snprintf(rows[nrows].value, sizeof(rows[nrows].value), "%u",
                 dropped);
        rows[nrows].name = "Dropped";
        rows[nrows].col = dropped == 0 ? HUD_TEXT
                        : dropped >= 8 ? HUD_BAD : HUD_WARN;
        nrows++;
    }

    /* Feed verdict, relative to the session's target fps (falls back to
     * the classic 60/30 readings when the target never landed). */
    {
        int target = live->target_fps > 0 ? live->target_fps : 60;
        const char *health_txt;
        uint32_t health_col;
        if (live->fps >= (float)target - 1.0f ||
            (live->fps >= 55.0f && target == 60)) {
            health_txt = "smooth";
            health_col = HUD_GOOD;
        } else if (live->fps >= (float)target * 0.5f) {
            health_txt = "keeping up";
            health_col = HUD_WARN;
        } else {
            health_txt = "behind";
            health_col = HUD_BAD;
        }
        snprintf(rows[nrows].value, sizeof(rows[nrows].value), "%s",
                 health_txt);
        rows[nrows].name = "Feed";
        rows[nrows].col = health_col;
        nrows++;
    }

    for (int i = 0; i < nrows; i++) {
        int y = ty + i * row_h;
        ui_label(s, px + pad, y, vl, 0, HUD_DIM, rows[i].name);
        int wv = ui_label_w(vv, 1, rows[i].value);
        /* Value (24) shares the name's (22) baseline. */
        ui_label(s, px + pw - pad - wv, y - (60 * (vv - vl) + 32) / 64, vv,
                 1, rows[i].col, rows[i].value);
    }

    /* Frame-time timeline: oldest left, newest right. Budget thresholds
     * follow the session fps when it is known (a 30 fps stream's bars
     * must not read as "half-rate" against a 60 fps yardstick). */
    int gx = px + pad, gy = ty + nrows * row_h + 8;
    ui_round_rect_alpha(s, gx, gy, graph_w, graph_h, 8, HUD_GRAPH_BG,
                        STATS_GRAPH_ALPHA);

    int target = live->target_fps > 0 ? live->target_fps : 60;
    float budget_ms = 1000.0f / (float)target;
    float max_ms = budget_ms; /* floor at the budget so a smooth feed still shows bars */
    for (int i = 0; i < live->frame_count; i++)
        if (live->frame_ms[i] > max_ms)
            max_ms = live->frame_ms[i];

    /* Budget guide line across the graph (the teal/green threshold
     * the bar colors use). */
    {
        int by = gy + graph_h - 3 -
                 (int)(budget_ms / max_ms * (graph_h - 6) + 0.5f);
        if (by > gy + 2)
            ui_rect(s, gx + 6, by, graph_w - 12, 1, hud_blend(HUD_GOOD,
                     HUD_GRAPH_BG, 150));
    }

    const int bar_w = 2, gap = 1;
    int nbars = graph_w / (bar_w + gap);
    if (nbars > live->frame_count)
        nbars = live->frame_count;
    int x0 = gx + graph_w - nbars * (bar_w + gap) - 1;
    for (int i = 0; i < nbars; i++) {
        int src_idx = live->frame_count - nbars + i;
        float ms = live->frame_ms[src_idx];
        int h = (int)(ms / max_ms * (graph_h - 6) + 0.5f);
        if (h > graph_h - 6) h = graph_h - 6;
        if (h < 1) h = 1;
        uint32_t col = HUD_GOOD;
        if (ms > budget_ms * 2.0f)
            col = HUD_BAD; /* two budgets over = clearly late */
        else if (ms > budget_ms)
            col = HUD_WARN; /* over budget */
        ui_rect(s, x0 + i * (bar_w + gap), gy + graph_h - 3 - h, bar_w, h, col);
    }
}

/* --- congestion warning pill --------------------------------------------------- */

/* One-time-per-session amber warning: the moment the live Dropped count
 * crosses the congestion threshold (renderer-side latch), this pill fades
 * in for ~4 s at the top center. It does not repeat within a session -
 * the perf overlay's Dropped row carries the running count instead. */
void stream_hud_warning(ui_surface_t *s, int alpha) {
    if (alpha <= 0)
        return;
    if (alpha > 220)
        alpha = 220;

    const char *text = "Network congestion - some frames are being dropped";
    const int size = 26;
    int tw = ui_label_w(size, 0, text);
    int pw = tw + 96;
    int ph = 68;
    int px = (s->w - pw) / 2;
    int py = 46;

    ui_round_rect_alpha(s, px, py, pw, ph, 12, HUD_BG_DEEP, alpha);
    /* Amber left edge, the system's "caution" accent. */
    ui_rect(s, px, py, 3, ph, HUD_WARN);
    /* The info badge shape recolored to amber marks it as a warning.
     * Text core centers on the pill's midline. */
    ui_icon_info(s, px + 44, py + ph / 2, 30, HUD_WARN);
    ui_label(s, px + 74, py + ph / 2 - ui_label_ink_cy(size, 0), size, 0,
             HUD_TEXT, text);
    (void)tw;
}

/* --- hold ring ------------------------------------------------------------------ */
void stream_hud_hold(ui_surface_t *s, int prog) {
    uint32_t accent = HUD_ACCENT;
    const char *label = "Stream menu";

    int cx = s->w / 2, cy = s->h - 190;
    int radius = 44, thick = 7;
    ui_ring_arc(s, cx, cy, radius, thick, HUD_TRACK, 0, 1000);
    ui_ring_arc(s, cx, cy, radius, thick, accent, 0, prog);
    /* Settings gear in the ring's center - the menu this hold opens. */
    ui_icon(s, cx - 17, cy - 17, 34, 5, HUD_TEXT);
    int lw = ui_label_w(24, 0, label);
    ui_label(s, cx - lw / 2, cy + radius + 18, 24, 0, HUD_TEXT, label);
}

/* --- combo hint (in-game prompt style) -------------------------------------------- */

/* Key strings render as the real PlayStation chips: a known button name
 * ("L1", "OPTIONS", ...) draws its keycap icon, any other word ("(hold)",
 * "+") draws as text. One walker serves the measure and draw passes so
 * the pill stays centered. */
static const struct {
    const char *name;
    int btn;
} k_hint_tokens[] = {
    { "L1", PSBTN_L1 },       { "R1", PSBTN_R1 },
    { "L2", PSBTN_L2 },       { "R2", PSBTN_R2 },
    { "L3", PSBTN_L3 },       { "R3", PSBTN_R3 },
    { "OPTIONS", PSBTN_OPTIONS }, { "SHARE", PSBTN_SQUARE },
    { "CROSS", PSBTN_CROSS },  { "CIRCLE", PSBTN_CIRCLE },
    { "TRIANGLE", PSBTN_TRIANGLE }, { "SQUARE", PSBTN_SQUARE },
    { "TOUCHPAD", PSBTN_TOUCHPAD },
};

#define HINT_TEXT 26
#define HINT_CHIP 30

/* Advance *p past one token; fills word (nul-terminated) and, when the
 * word is a known button, its chip id. Returns 0 at end of string. */
static int hint_next(const char **p, char *word, size_t cap, int *btn) {
    const char *s = *p;
    while (*s == ' ')
        s++;
    if (!*s) {
        *p = s;
        return 0;
    }
    size_t n = 0;
    while (*s && *s != ' ' && n + 1 < cap)
        word[n++] = *s++;
    while (*s && *s != ' ')
        s++;
    word[n] = 0;
    *p = s;
    *btn = -1;
    for (size_t i = 0; i < sizeof(k_hint_tokens) / sizeof(k_hint_tokens[0]);
         i++) {
        if (!strcmp(word, k_hint_tokens[i].name)) {
            *btn = k_hint_tokens[i].btn;
            break;
        }
    }
    return 1;
}

static int hint_keys_w(const char *keys) {
    const char *p = keys;
    char word[24];
    int btn, w = 0, first = 1;
    while (hint_next(&p, word, sizeof(word), &btn)) {
        if (!first)
            w += 10;
        first = 0;
        w += btn < 0 ? ui_label_w(HINT_TEXT, 1, word)
                     : ui_ps_btn_w(HINT_CHIP, btn);
    }
    return w;
}

static int hint_keys_draw(ui_surface_t *s, int x, int y, const char *keys,
                          int alpha) {
    const char *p = keys;
    char word[24];
    int btn, first = 1;
    while (hint_next(&p, word, sizeof(word), &btn)) {
        if (!first)
            x += 10;
        first = 0;
        if (btn < 0) {
            ui_label(s, x, y, HINT_TEXT, 1, HUD_TEXT, word);
            x += ui_label_w(HINT_TEXT, 1, word);
        } else {
            /* Chip centered on the text run's ink midline (words render
             * bold here, so the bold band's midline is the one to ride). */
            ui_ps_btn(s, x, y + ui_label_ink_cy(HINT_TEXT, 1) - HINT_CHIP / 2,
                      HINT_CHIP, btn, alpha);
            x += ui_ps_btn_w(HINT_CHIP, btn);
        }
    }
    return x;
}

void stream_hud_hint(ui_surface_t *s, const stream_hint_t *hints, int nhints,
                    int alpha) {
    if (alpha <= 0 || !hints)
        return;
    if (alpha > HINT_PILL_ALPHA)
        alpha = HINT_PILL_ALPHA;

    const int size_a = 26;
    int total_w = 0;
    for (int i = 0; i < nhints; i++) {
        total_w += hint_keys_w(hints[i].keys) + 16 +
                   ui_label_w(size_a, 0, hints[i].action) + (i ? 34 : 0);
    }
    if (total_w <= 0)
        return;

    int ph = 76, py = s->h - ph - 30;
    int px = (s->w - total_w) / 2;
    /* PS4's in-game hint surfaces are softly rounded panels, not fully
     * rounded capsules (that reads PS5-era); 16 px keeps the softness. */
    ui_round_rect_alpha(s, px - 36, py, total_w + 72, ph, 16, HUD_BG,
                        alpha);

    int x = px;
    /* One ink midline for the whole pill: chips and text cores all
     * center on it (the old (ph-34)/2 top sat the text 3 px low). */
    int mid = py + ph / 2;
    int ty = mid - ui_label_ink_cy(size_a, 0);
    for (int i = 0; i < nhints; i++) {
        x = hint_keys_draw(s, x, ty, hints[i].keys, alpha);
        x += 16;
        ui_label(s, x, ty, size_a, 0, HUD_DIM, hints[i].action);
        x += ui_label_w(size_a, 0, hints[i].action) + 34;
    }
}

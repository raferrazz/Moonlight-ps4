/* ==========================================================================
 * Orbis native layout — the one shell every theme flows through.
 *
 * Replaces the per-theme layout zoo (six Games grids, two sidebars, four
 * footers). Zones follow the PS4 reference captures: header wordmark +
 * tab strip + hairline, a single content zone, the PS function row at
 * the bottom, white 2px focus frames on rows, an enlarged hero tile with
 * the primary action attached inside its bottom edge, and top-right
 * notification toasts. Themes only change palette + background.
 *
 * Layout fixes, verified against rendered captures:
 *  - Games hero row rebuilt: small tiles are TOP-aligned (they used to
 *    share the hero's center line, which pushed them into the title
 *    block - the "Cinderfall Tactics" overlap), packed at a constant 24
 *    px gap like the reference row, and the title block moved below the
 *    small-tile band so the focused game's name lives in clear space.
 *  - The action bar is the reference "Iniciar" treatment: full-width
 *    dark translucent bar flush with the tile's bottom edge, centered
 *    white verb, no glyph.
 *  - Every button affordance in the shell (function row, OSK footer,
 *    empty states, hint lines) renders the real PlayStation chip icons
 *    (ui_ps_btn) instead of drawn vectors.
 *  - OSK panel geometry is computed from its content, so the keypad
 *    always fits the panel and the screen (the old fixed sizes pushed
 *    the bottom key rows off-screen). The hex keypad is 6x3 (wide) so
 *    both keyboards share one compact native panel shape.
 *
 * Included by ui_menu.c; expects ui_state_t, COL_* macros, the settings
 * geometry macros and the helpers in that file to be in scope.
 * ========================================================================== */

/* --- shared content helpers (moved from the retired per-theme file) --- */

/* when the selected game is ALREADY running on the host (currentGame),
 * the primary action is to resume it, not relaunch. */
static const char *design_action_label(ui_state_t *st) {
    if (st->server && st->server->currentGame && st->napps > 0 &&
        st->apps[st->sel_app].id == st->server->currentGame)
        return "Resume";
    return "Play";
}

static void design_profile(ui_state_t *st, char *text, size_t size) {
    snprintf(text, size, "%dp / %d FPS target", st->cfg->stream.height,
             st->cfg->stream.fps);
}

/* --- small helpers ------------------------------------------------------- */

static uint32_t blend_col(uint32_t a, uint32_t b, int amount) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int av = (a >> shift) & 255, bv = (b >> shift) & 255;
        out |= (uint32_t)(av + (bv - av) * amount / 255) << shift;
    }
    return out;
}

static uint32_t orbis_sep(void) { /* hairline separators */
    return blend_col(ui_theme_current()->text, ui_theme_current()->bg, 40);
}

/* Fractional selection for the Games hero row: eased toward sel_app by
 * the menu loop (which stays dirty until settled). */
static double orbis_sel_f(const ui_state_t *st) { return st->sel_f; }

/* Horizontal slide offset for tab switches (0 when settled). */
static int orbis_view_dx(const ui_state_t *st) {
    if (!st->tab_anim_ms)
        return 0;
    unsigned long long el = menu_now_ms() - st->tab_anim_ms;
    if (el >= 180)
        return 0;
    int inv = (int)(180 - el);
    int mag = 36 * inv * inv / (180 * 180); /* quadratic ease-out */
    return st->tab_dir >= 0 ? -mag : mag;
}

/* --- composed hint lines: text runs + PlayStation chips in one row -----
 * The reference PS4 draws button affordances as the real keycap chips
 * inline with the label text; these helpers measure and draw such rows
 * so they can be centered like plain labels. */
typedef struct {
    const char *text; /* drawn when btn < 0 */
    int btn;          /* PSBTN_* chip when >= 0 */
} orbis_seg_t;
#define ORBIS_TXT(t)   { (t), -1 }
#define ORBIS_BTN(b)   { NULL, (b) }

static int orbis_segs_w(const orbis_seg_t *seg, int n, int size) {
    int chip_h = size + 6;
    int w = 0;
    for (int i = 0; i < n; i++) {
        w += seg[i].btn < 0 ? ui_label_w(size, 0, seg[i].text)
                            : ui_ps_btn_w(chip_h, seg[i].btn);
        if (i + 1 < n)
            w += 12;
    }
    return w;
}

static void orbis_segs(ui_surface_t *s, int x, int y, const orbis_seg_t *seg,
                       int n, int size, uint32_t col, int alpha) {
    int chip_h = size + 6;
    for (int i = 0; i < n; i++) {
        if (seg[i].btn < 0) {
            ui_label(s, x, y, size, 0, col, seg[i].text);
            x += ui_label_w(size, 0, seg[i].text);
        } else {
            /* chip centered on the text run's ink midline */
            ui_ps_btn(s, x, y + ui_label_ink_cy(size, 0) - chip_h / 2,
                      chip_h, seg[i].btn, alpha);
            x += ui_ps_btn_w(chip_h, seg[i].btn);
        }
        x += 12;
    }
}

/* PS function-row entry: real chip + label, at any y (chip top). The
 * label's ink core centers on the chip's midline - the pair reads as
 * one optically centered unit (the old +5 offset sat text 4 px low). */
static int orbis_fn_at(ui_surface_t *s, int x, int y, const char *label,
                       int btn, int enabled) {
    const int chip_h = 30;
    int cw = ui_ps_btn_w(chip_h, btn);
    ui_ps_btn(s, x, y, chip_h, btn, enabled ? 255 : 130);
    ui_label(s, x + cw + 14, y + chip_h / 2 - ui_label_ink_cy(22, 0), 22, 0,
             enabled ? COL_TEXT : COL_DIM, label);
    return x + cw + 14 + ui_label_w(22, 0, label) + 60;
}

/* --- empty / quiet states (the reference "no content" pattern) --------- */

/* Centered working state: the system spinner above the copy. */
static void orbis_working_state(ui_surface_t *s, int dx, const char *title,
                                const char *sub) {
    int cx = 960 + dx;
    ui_spinner(s, cx, 372, 34, (int)(menu_now_ms() / 40), COL_DIM);
    ui_label(s, cx - ui_label_w(34, 1, title) / 2, 470, 34, 1, COL_TEXT,
             title);
    if (sub && sub[0])
        ui_label(s, cx - ui_label_w(24, 0, sub) / 2, 542, 24, 0, COL_DIM,
                 sub);
}

/* Centered message state with a chip-composed action line. */
static void orbis_empty_state(ui_surface_t *s, int dx, const char *title,
                              const char *sub, const orbis_seg_t *acts,
                              int n_acts) {
    int cx = 960 + dx;
    ui_label(s, cx - ui_label_w(34, 1, title) / 2, 458, 34, 1, COL_TEXT,
             title);
    if (sub && sub[0])
        ui_label(s, cx - ui_label_w(24, 0, sub) / 2, 522, 24, 0, COL_DIM,
                 sub);
    if (acts && n_acts > 0)
        orbis_segs(s, cx - orbis_segs_w(acts, n_acts, 24) / 2, 590, acts,
                   n_acts, 24, COL_ACCENT, 255);
}

static void orbis_art(const ui_state_t *st, int index, ui_surface_t *s,
                      int x, int y, int w, int h) {
    if (!strcasecmp(st->apps[index].name, "Desktop")) {
        ui_landscape(s, x, y, w, h, 10);
        int icon = h > 300 ? 96 : h > 120 ? 60 : 30;
        ui_icon(s, x + w / 2 - icon / 2, y + h / 2 - icon / 2, icon, 0,
                0xfff4f9ff);
    } else {
        ui_art_draw_round(st->server, st->apps[index].id, s, x, y, w, h, 10);
    }
}

/* --- background / header / footer ---------------------------------------- */

static void orbis_shell(ui_state_t *st, ui_surface_t *s) {
    ui_theme_background(s);

    const char *tabs[] = { "PCs", "Games", "Settings" };

    /* Wordmark. */
    ui_label_caps(s, 64, 46, 34, COL_TEXT, "Moonlight", 7);

    /* Right-aligned connection chips. */
    int connected = st->server && st->server->paired;
    {
        const char *txt = connected ? "Connected" : "Not connected";
        int w = ui_label_w(24, 0, txt);
        /* Status dot: a bullet rides the text's x-height center (the
         * lowercase body's midline - master row 45), the way console
         * status indicators sit against mixed-case labels. */
        ui_circle(s, 1856 - w - 20, 48 + (45 * 24 + 32) / 64, 6,
                  connected ? 0xff42cc94 : ui_theme_current()->warn);
        ui_label(s, 1856 - w, 48, 24, 0, COL_DIM, txt);
        if (st->server && st->server->currentGame) {
            const char *an = active_app_name(st);
            char chip[96];
            snprintf(chip, sizeof(chip), "Streaming  \xc2\xb7  %s",
                     an ? an : "a game");
            int w2 = ui_label_w(22, 0, chip);
            ui_label(s, 1856 - w2, 88, 22, 0, COL_ACCENT, chip);
        }
    }

    /* Tab strip. */
    {
        int x = 64;
        for (int i = 0; i < 3; i++) {
            int sel = (int)st->tab == i;
            ui_label(s, x, 116, 30, sel, sel ? COL_TEXT : COL_DIM, tabs[i]);
            int w = ui_label_w(30, sel, tabs[i]);
            if (sel)
                ui_rect(s, x + 2, 158, w - 4, 3, COL_ACCENT);
            x += w + 64;
        }
    }

    /* Hairline under the header. */
    ui_rect(s, 64, 178, 1792, 1, orbis_sep());
}

static void orbis_footer(ui_state_t *st, ui_surface_t *s) {
    ui_rect(s, 64, 990, 1792, 1, orbis_sep());

    int x = 64;
    const int fy = 1004;
    if (st->tab == TAB_APPS)
        x = orbis_fn_at(s, x, fy, design_action_label(st), PSBTN_CROSS,
                        st->napps > 0);
    else if (st->tab == TAB_PCS) {
        /* PC rows connect; the Add row adds - same button, the row
         * decides. */
        x = orbis_fn_at(s, x, fy,
                        st->sel_host < st->nhosts ? "Connect" : "Add PC",
                        PSBTN_CROSS, 1);
    } else
        x = orbis_fn_at(s, x, fy, "Edit", PSBTN_CROSS, 1);
    x = orbis_fn_at(s, x, fy, "Back", PSBTN_CIRCLE, 1);
    x = orbis_fn_at(s, x, fy,
                    st->tab == TAB_SETTINGS ? "Change" : "Refresh",
                    PSBTN_TRIANGLE, 1);
    if (st->tab == TAB_APPS && st->napps)
        x = orbis_fn_at(s, x, fy, "Default", PSBTN_SQUARE, 1);
    if (st->tab == TAB_PCS && st->sel_host < st->nhosts) {
        x = orbis_fn_at(s, x, fy, "Wake PC", PSBTN_SQUARE, 1);
        x = orbis_fn_at(s, x, fy, "Forget", PSBTN_OPTIONS, 1);
    }

    /* Right: the tab-switch affordance, PS4 keycap chips + label.
     * Same optical midline as the left function chips (fy + 15). */
    {
        orbis_seg_t seg[3] = {
            ORBIS_BTN(PSBTN_L1), ORBIS_BTN(PSBTN_R1), ORBIS_TXT("Tabs")
        };
        int w = orbis_segs_w(seg, 3, 22);
        orbis_segs(s, 1856 - w, fy + 15 - ui_label_ink_cy(22, 0), seg, 3, 22,
                   COL_DIM, 255);
    }

    /* Status line just above the separator. The toast is the event view
     * of a notice; this line only carries standing, actionable states
     * (repeating the toast text here read as duplicate error spam
     * against rendered captures). */
    if (st->host_dirty) {
        orbis_seg_t seg[3] = {
            ORBIS_TXT("Host changed - open Games and press "),
            ORBIS_BTN(PSBTN_TRIANGLE),
            ORBIS_TXT(" to reconnect.")
        };
        orbis_segs(s, 64, 952, seg, 3, 22, ui_theme_current()->warn, 255);
    } else if (st->server && st->server->currentGame) {
        const char *an = active_app_name(st);
        char note[96];
        snprintf(note, sizeof(note), "   Session: %s",
                 an ? an : "your game");
        orbis_seg_t seg[2] = { ORBIS_BTN(PSBTN_OPTIONS),
                               ORBIS_TXT("") };
        seg[1].text = note;
        orbis_segs(s, 64, 952, seg, 2, 22, COL_DIM, 255);
    }
}

/* --- toast (native notification pattern) ----------------------------------- */

#define TOAST_W 560
#define TOAST_MS 4200

/* Balanced two-line split of one label: break at the space whose halves
 * differ least in width, both within max_w. Returns 0 when the text fits
 * one line or no balanced split exists (the caller then keeps its
 * single-line handling). Shared by the toast and the settings row hints
 * - one wrap rule everywhere. */
static int orbis_wrap_balanced(const char *text, int size, int max_w,
                               char *l1, size_t n1, char *l2, size_t n2) {
    int total = ui_label_w(size, 0, text);
    if (total <= max_w || !strchr(text, ' '))
        return 0;
    int best = -1, best_gap = 1 << 30;
    const char *p = text;
    while ((p = strchr(p, ' ')) != NULL) {
        int off = (int)(p - text);
        int w1 = total - ui_label_w(size, 0, p);
        int w2 = ui_label_w(size, 0, p + 1);
        int gap = w1 > w2 ? w1 - w2 : w2 - w1;
        if (w1 <= max_w && w2 <= max_w && gap < best_gap) {
            best = off;
            best_gap = gap;
        }
        p++;
    }
    if (best < 0)
        return 0;
    snprintf(l1, n1, "%.*s", best, text);
    snprintf(l2, n2, "%s", text + best + 1);
    return ui_label_w(size, 0, l2) <= max_w ? 2 : 0;
}

/* Split a long notice into two balanced lines (PS4 notifications wrap;
 * the old single line truncated mid-word: "connection refused (h..."). */
static int toast_split(const char *text, char *l1, size_t n1, char *l2,
                       size_t n2, int max_w) {
    return orbis_wrap_balanced(text, 24, max_w, l1, n1, l2, n2);
}

static void orbis_toast(ui_state_t *st, ui_surface_t *s) {
    if (!st->toast_ms || !st->notice[0])
        return;
    unsigned long long el = menu_now_ms() - st->toast_ms;
    if (el > TOAST_MS + 250)
        return;
    int alpha = 225;
    int xoff = 0;
    if (el > TOAST_MS) {
        alpha = 225 - (int)((el - TOAST_MS) * 225 / 250);
    } else if (el < 250) {
        int t = (int)el;
        xoff = (TOAST_W + 60) * (250 - t) * (250 - t) / (250 * 250);
    }
    if (alpha <= 0)
        return;

    const ui_theme_t *t = ui_theme_current();
    char l1[160], l2[160];
    int lines = toast_split(st->notice, l1, sizeof(l1), l2, sizeof(l2),
                            TOAST_W - 100);
    int h = lines == 2 ? 128 : 84;
    int x = 1920 - TOAST_W - 56 + xoff;
    int y = 208;
    /* Translucency + a nearly-invisible edge: a 250-alpha panel with
     * a brighter 1px hairline reads as an opaque "web alert". System
     * notification panels let the theme ghost through (~88%) and carry
     * no strong outline. */
    ui_round_rect_alpha(s, x, y, TOAST_W, h, 10, t->panel, alpha);
    uint32_t hair = blend_col(t->focus, t->panel, 25);
    ui_frame(s, x, y, TOAST_W, h, hair, 1);

    /* Notification glyph: the info badge from the reference set - solid
     * disc with a knocked-out "i" - tinted with the theme accent, so
     * every theme carries its own notification color. */
    ui_icon_info(s, x + 34, y + h / 2, 32, COL_ACCENT);

    ui_label_set_alpha(alpha);
    if (lines == 2) {
        /* Both lines' ink cores straddle the panel's midline
         * (core centers at h/2 - 20 and h/2 + 20). */
        ui_label(s, x + 62, y + h / 2 - 20 - ui_label_ink_cy(24, 0), 24, 0,
                 COL_TEXT, l1);
        ui_label(s, x + 62, y + h / 2 + 20 - ui_label_ink_cy(24, 0), 24, 0,
                 COL_TEXT, l2);
    } else {
        ui_label(s, x + 62, y + h / 2 - ui_label_ink_cy(24, 0), 24, 0,
                 COL_TEXT, st->notice);
    }
    ui_label_set_alpha(255);
}

/* --- Games tab: the dynamic-menu hero row ---------------------------------- */

#define HERO_L 64      /* focused tile left edge when settled */
#define HERO_W 448     /* focused tile size (square) */
#define HERO_TOP 312   /* row top: tiles grow DOWN as they gain focus */
#define TILE_W 208     /* small-tile size (~46% of the hero, like home) */
#define ROW_GAP 24     /* constant gap between every tile, ref ~15-24 px */

/* Slot width while focus glides: tiles interpolate between the small
 * size and the hero size by their closeness to the (fractional)
 * selection, so the row never overlaps at any point of the glide. */
static double orbis_slot_w(double f, int i) {
    double rel = (double)i - f;
    if (rel < 0)
        rel = -rel;
    double close = 1.0 - rel;
    if (close < 0)
        close = 0;
    return (double)TILE_W + (double)(HERO_W - TILE_W) * close;
}

static void orbis_games(ui_state_t *st, ui_surface_t *s) {
    int dx = orbis_view_dx(st);

    if (!st->napps) {
        if (st->refresh_pending) {
            orbis_working_state(s, dx, "Loading games",
                                "Fetching the list from your PC; this can "
                                "take a few seconds.");
        } else {
            /* Quiet state: the copy carries the refresh affordance once
             * (the function row repeats it anyway) - the second
             * "Refresh" prompt under the message read as duplication. */
            orbis_seg_t sub[3] = {
                ORBIS_TXT("Press "), ORBIS_BTN(PSBTN_TRIANGLE),
                ORBIS_TXT(" to refresh, or check your PC connection.")
            };
            int cx = 960 + dx;
            const char *title = st->apps_err ? "Unable to load games"
                                             : "No games available";
            ui_label(s, cx - ui_label_w(40, 1, title) / 2, 446, 40, 1,
                     COL_TEXT, title);
            orbis_segs(s, cx - orbis_segs_w(sub, 3, 27) / 2, 518, sub, 3,
                       27, COL_DIM, 255);
        }
        return;
    }

    double f = orbis_sel_f(st);
    int k = (int)f;
    if (k >= st->napps - 1)
        k = st->napps - 2;
    if (k < 0)
        k = 0;
    double g = f - k; /* 0 = settled on k, 1 = settled on k+1 */

    /* Centers: the two slots straddling the glide point keep their
     * midpoint on a linear path between the settled midpoints, so the
     * pair stays centered while both sizes interpolate; everything else
     * packs outward at the constant row gap - the packed-row geometry
     * the reference uses. Verified: no slot pair ever overlaps. */
    double sk = orbis_slot_w(f, k);
    double sk1 = orbis_slot_w(f, k + 1);
    double mid_cx = (double)(HERO_L + HERO_W / 2) +
                    (double)(HERO_W / 2 + ROW_GAP + TILE_W / 2) * (0.5 - g);
    double d = (sk + sk1) / 4.0 + (double)ROW_GAP / 2.0;
    double c[10];
    int base = k - 3;
    c[k - base] = mid_cx - d;
    c[k + 1 - base] = mid_cx + d;
    for (int i = k - 1; i >= base; i--)
        c[i - base] = c[i + 1 - base] -
                      (orbis_slot_w(f, i) + orbis_slot_w(f, i + 1)) / 2.0 -
                      (double)ROW_GAP;
    for (int i = k + 2; i <= base + 9 && i < st->napps; i++)
        c[i - base] = c[i - 1 - base] +
                      (orbis_slot_w(f, i - 1) + orbis_slot_w(f, i)) / 2.0 +
                      (double)ROW_GAP;

    int i0 = base < 0 ? 0 : base;
    int i1 = base + 9;
    if (i1 >= st->napps)
        i1 = st->napps - 1;
    for (int i = i0; i <= i1; i++) {
        /* Snap the glide sweep onto the art render-cache ladder: sizes
         * animate in 32 px steps (endpoints exact), every (app, size)
         * is scaled once and then blitted - resampling ~10 tiles per
         * frame through the 64-bit bilinear is what kept the console
         * row under 60 fps (the PS4 dashboard translates its tiles
         * pixel-identical while scrolling). The
         * settled row is pixel-identical to the continuous math: the
         * endpoints ARE TILE_W and HERO_W. */
        int size = ui_art_ladder_snap((int)(orbis_slot_w(f, i) + 0.5));
        int x = (int)(c[i - base] - (double)size / 2.0) + dx;
        int y = HERO_TOP;
        if (x + size < -8 || x > 1928)
            continue;
        orbis_art(st, i, s, x, y, size, size);
        if (i == st->sel_app) {
            /* Hero: soft glow + crisp white ring around the whole tile
             * (the ring wraps the action bar too, like the reference). */
            ui_focus(s, x, y, size, size, 10, COL_ACCENT, 8);
            ui_focus(s, x, y, size, size, 10, ui_theme_current()->focus, 0);
            /* Reference "Iniciar" bar: full-width dark translucent
             * overlay flush with the tile's bottom edge, centered white
             * verb, no glyph. */
            const char *verb = design_action_label(st);
            int bar_h = 72;
            ui_round_rect_alpha(s, x, y + size - bar_h, size, bar_h, 0,
                                0xFF000000, 150);
            ui_rect(s, x, y + size - bar_h, size, 1, 0x40FFFFFF);
            int lw = ui_label_w(30, 1, verb);
            ui_label(s, x + (size - lw) / 2, y + size - bar_h +
                      (bar_h - 38) / 2, 30, 1, 0xFFFFFFFF, verb);
        }
    }

    /* Focused game's name + meta: right of the hero, below the small
     * tile band (the reference puts the large title in the clear space
     * under the neighboring row, never over the tiles). The block
     * cross-fades while the row glides. */
    {
        double dist = f - (double)st->sel_app;
        if (dist < 0)
            dist = -dist;
        int alpha = (int)(255.0 * (1.0 - 2.0 * dist));
        if (alpha < 0)
            alpha = 0;
        if (alpha > 255)
            alpha = 255;
        ui_label_set_alpha(alpha);

        int tx = HERO_L + HERO_W + 56 + dx;
        ui_label_fit(s, tx, 556, 60, 0, COL_TEXT, st->apps[st->sel_app].name,
                     1856 - tx);
        char meta[192];
        char profile[80];
        design_profile(st, profile, sizeof(profile));
        int is_default = st->cfg->autostart &&
                         atoi(st->cfg->app_name) == st->apps[st->sel_app].id;
        snprintf(meta, sizeof(meta), "%d of %d  \xc2\xb7  %s%s",
                 st->sel_app + 1, st->napps, profile,
                 is_default ? "  \xc2\xb7  AUTO-START" : "");
        ui_label(s, tx, 652, 24, 0, COL_DIM, meta);
        if (st->server && st->server->currentGame &&
            st->apps[st->sel_app].id == st->server->currentGame) {
            ui_circle(s, tx + 8, 700 + (45 * 24 + 32) / 64, 6, 0xff42cc94);
            ui_label(s, tx + 28, 700, 24, 0, COL_DIM, "Running on your PC");
        }
        ui_label_set_alpha(255);
    }
}

/* --- PCs tab --------------------------------------------------------------- */

static void orbis_pcs(ui_state_t *st, ui_surface_t *s) {
    int dx = orbis_view_dx(st);

    ui_label(s, 64 + dx, 234, 36, 1, COL_TEXT, "Your computers");
    ui_label(s, 64 + dx, 290, 24, 0, COL_DIM,
             "Computers you connect to are remembered here.");

    /* Standing sub-line: what the scan is doing while it runs, or the
     * list's capacity. The toast reports the outcome. */
    if (st->scan) {
        char line[96];
        snprintf(line, sizeof(line), "Checking... %s",
                 st->scan_phase[0] ? st->scan_phase : "working");
        ui_label(s, 64 + dx, 336, 22, 0, COL_ACCENT, line);
    } else if (st->nhosts) {
        char line[96];
        snprintf(line, sizeof(line), "%d of %d computers", st->nhosts,
                 CONFIG_MAX_PCS);
        ui_label(s, 64 + dx, 336, 22, 0, COL_DIM, line);
    } else {
        ui_label(s, 64 + dx, 336, 22, 0, COL_DIM,
                 "Enter your PC's IP address to add it.");
    }

    /* Empty + scanning: the system spinner carries the wait (a dead-quiet
     * tab while TRIANGLE's probe runs read as a broken button - the
     * scan takes as long as the console's TCP budget against a powered
     * -off PC). */
    if (st->scan && !st->nhosts) {
        orbis_working_state(s, dx, "Checking for your PCs",
                            "Probing the computers you saved - this can "
                            "take a while when one is off.");
        return;
    }

    /* PC rows (MRU-first), then the in-place Add row. */
    int nrows = st->nhosts;
    int first = st->sel_host >= 6 ? st->sel_host - 5 : 0;
    for (int i = first; i < nrows && i < first + 6; i++) {
        int y = 400 + (i - first) * 88;
        int x = 64 + dx;
        int w = 1792;
        int sel = i == st->sel_host;
        if (sel)
            ui_frame(s, x, y, w, 72, ui_theme_current()->focus, 2);

        char name[64];
        const char *end = strstr(st->hosts[i].instance, "._nvstream");
        size_t len = end ? (size_t)(end - st->hosts[i].instance)
                         : strlen(st->hosts[i].instance);
        if (len >= sizeof(name))
            len = sizeof(name) - 1;
        memcpy(name, st->hosts[i].instance, len);
        name[len] = 0;

        /* monitor pictogram: real asset, tinted, centered in its box.
         * Name, status and address all ride the row midline (y + 36):
         * icon center y + 36, label cores at y + 36 via ink_cy offsets
         * (the selected row renders bold, so its band rides the bold
         * midline). */
        ui_icon(s, x + 20, y + 15, 42, 0, sel ? COL_TEXT : COL_DIM);
        ui_label_fit(s, x + 88, y + 36 - ui_label_ink_cy(30, sel), 30, sel,
                     sel ? COL_TEXT : COL_DIM, name, 560);
        int connected = st->server && st->server->paired &&
                        !strcmp(st->server->address, st->hosts[i].address);
        int online = st->hosts[i].online;
        const char *status = connected ? "Connected"
                          : online == 1 ? "Online"
                          : online == 0 ? "Offline"
                          : "Checking...";
        uint32_t dot = connected || online == 1 ? 0xff42cc94
                     : online == 0 ? ui_theme_current()->warn : COL_DIM;
        ui_circle(s, x + 700, y + 36, 7, dot);
        ui_label(s, x + 720, y + 36 - ui_label_ink_cy(24, 0), 24, 0,
                 connected ? COL_ACCENT : sel ? COL_TEXT : COL_DIM, status);
        char line[80];
        snprintf(line, sizeof(line), "%s:%u", st->hosts[i].address,
                 (unsigned)st->hosts[i].port);
        ui_label(s, x + w - 32 - ui_label_w(24, 0, line),
                 y + 36 - ui_label_ink_cy(24, 0), 24, 0, COL_DIM, line);
    }
    /* Add PC row (in place, below the list; selectable like a PC). */
    {
        int idx = nrows;
        int y = 400 + (idx - first) * 88;
        if (idx >= first && idx < first + 6) {
            int x = 64 + dx;
            int w = 1792;
            int sel = idx == st->sel_host;
            if (sel)
                ui_frame(s, x, y, w, 72, ui_theme_current()->focus, 2);
            /* Plus keycap chip: rounded square with a plus, the system
             * "add" treatment; label + hint ride the row midline. */
            uint32_t ink = sel ? COL_TEXT : COL_DIM;
            int cb = 34, cx = x + 24, cy2 = y + 36;
            ui_round_rect_alpha(s, cx, cy2 - cb / 2, cb, cb, 8,
                                blend_col(ui_theme_current()->panel,
                                          ui_theme_current()->bg, 120), 200);
            ui_rect(s, cx + cb / 2 - 11, cy2 - 2, 22, 4, ink);
            ui_rect(s, cx + cb / 2 - 2, cy2 - 11, 4, 22, ink);
            ui_label(s, x + 88, y + 36 - ui_label_ink_cy(30, sel), 30, sel,
                     sel ? COL_TEXT : COL_DIM, "Add PC");
            ui_label(s, x + 300, y + 36 - ui_label_ink_cy(22, 0), 22, 0,
                     COL_DIM, "Enter your PC's IP address");
        }
    }
    if (PCS_ROWS(st) > 6) {
        char count[32];
        snprintf(count, sizeof(count), "%d of %d", st->sel_host + 1,
                 PCS_ROWS(st));
        ui_label(s, 1856 - ui_label_w(22, 0, count), 956, 22, 0, COL_DIM,
                 count);
    }
}

/* --- native confirmation dialog (PCs tab: Forget) --------------------------- */

static void orbis_confirm(ui_state_t *st, ui_surface_t *s) {
    if (!st->confirm_active)
        return;
    int idx = st->confirm_index;
    if (idx < 0 || idx >= st->nhosts)
        return;

    /* Same native-dialog pattern as the launch fail screen: deep scrim,
     * framed panel, centered copy, function-row verbs inside. */
    ui_scrim(s, 0xFF000000, 170);
    const int pw = 1000, ph = 420;
    const int px = (1920 - pw) / 2, py = 240;
    const ui_theme_t *t = ui_theme_current();
    ui_round_rect_alpha(s, px, py, pw, ph, 14, t->panel, 250);
    ui_frame(s, px, py, pw, ph, t->focus, 2);

    ui_label(s, 1920 / 2 - ui_label_w(40, 1, "Forget this PC?") / 2, py + 64,
             40, 1, COL_TEXT, "Forget this PC?");

    char line[160];
    const char *host = st->hosts[idx].address;
    const char *name = st->hosts[idx].instance;
    const char *end = strstr(name, "._nvstream");
    size_t len = end ? (size_t)(end - name) : strlen(name);
    if (len > 48)
        len = 48;
    char clean[52];
    memcpy(clean, name, len);
    clean[len] = 0;
    if (clean[0] && strcmp(clean, "Gaming PC") != 0)
        snprintf(line, sizeof(line), "%s (%s) is removed from this list.",
                 clean, host);
    else
        snprintf(line, sizeof(line), "%s is removed from this list.", host);
    ui_label_fit(s, 1920 / 2 - ui_label_w(26, 0, line) / 2, py + 150, 26, 0,
                 COL_DIM, line, pw - 120);
    ui_label_fit(s, 1920 / 2 - ui_label_w(24, 0,
                 "The PC can be added back at any time.") / 2, py + 200, 24,
                 0, COL_DIM, "The PC can be added back at any time.",
                 pw - 120);

    /* Action row: X Forget / O Cancel - real chips, ink-midline pairs. */
    {
        struct { const char *label; int btn; } acts[2] = {
            { "Forget", PSBTN_CROSS },
            { "Cancel", PSBTN_CIRCLE },
        };
        const int ch = 34;
        int total = 0;
        for (int kk = 0; kk < 2; kk++) {
            total += ui_ps_btn_w(ch, acts[kk].btn) + 14 +
                     ui_label_w(26, 0, acts[kk].label) + (kk ? 0 : 64);
        }
        int ax = 1920 / 2 - total / 2;
        for (int kk = 0; kk < 2; kk++) {
            int cw = ui_ps_btn_w(ch, acts[kk].btn);
            int cy = py + ph - 104 + ch / 2;
            ui_ps_btn(s, ax, cy - ch / 2, ch, acts[kk].btn, 255);
            ui_label(s, ax + cw + 14, cy - ui_label_ink_cy(26, 0), 26, 0,
                     kk == 0 ? ui_theme_current()->warn : COL_TEXT,
                     acts[kk].label);
            ax += cw + 14 + ui_label_w(26, 0, acts[kk].label) + 64;
        }
    }
}

/* --- Settings tab ------------------------------------------------------------ */

static void orbis_settings(ui_state_t *st, ui_surface_t *s) {
    int dx = orbis_view_dx(st);
    int x = 64 + dx, w = 1792;

    for (int i = 0; i < VISIBLE_SETTINGS; i++) {
        int row_y = SET_VIEW_Y0 + i * SET_ROW_H - st->set_scroll;
        if (row_y < SET_VIEW_Y0 - 1 ||
            row_y + SET_ROW_H > SET_VIEW_Y0 + SET_VIEW_H + 1)
            continue;
        int sel = i == st->sel_set;
        if (sel)
            ui_frame(s, x, row_y, w, SET_ROW_H - 14,
                     ui_theme_current()->focus, 2);
        ui_label(s, x + 32, row_y + 22, 30, sel, sel ? COL_TEXT : COL_DIM,
                 k_set_names[k_visible_settings[i]]);
        if (sel && !st->osk_active) {
            /* Contextual hint for the focused row - suppressed while the
             * OSK modal is up: the long one-liner ran behind the dialog
             * and read as a rendering artifact where the panel edge cut
             * it mid-word. The modal carries its own context line.
             * Long hints wrap to two balanced lines inside the row
             * (the single-line fitter used to ellipsize them). */
            const char *hint = set_row_hint(st, k_visible_settings[i]);
            if (hint) {
                char l1[160], l2[160];
                if (orbis_wrap_balanced(hint, 21, w - 64, l1, sizeof(l1),
                                       l2, sizeof(l2)) == 2) {
                    ui_label(s, x + 32, row_y + 58, 21, 0, COL_DIM, l1);
                    ui_label(s, x + 32, row_y + 84, 21, 0, COL_DIM, l2);
                } else {
                    ui_label_fit(s, x + 32, row_y + 62, 21, 0, COL_DIM, hint,
                                 w - 64);
                }
            }
        }
        char val[192];
        set_value_str(st, k_visible_settings[i], val, sizeof(val));
        /* Value shares the label's ink core (baseline): the 28 px value
         * top lands so its baseline matches the 30 px label's - the old
         * +26 offset dropped it 4 px below the row's text line. */
        ui_label_fit(s, x + w - 32 - ui_label_w(28, 0, val),
                     row_y + 22 + (60 * (30 - 28) + 32) / 64, 28, 0,
                     sel ? COL_TEXT : COL_DIM, val, 900);
    }

    /* Right-edge scroll indicator. */
    int max_top = (VISIBLE_SETTINGS - SET_VISIBLE_ROWS) * SET_ROW_H;
    if (max_top > 0) {
        int track_h = SET_VIEW_H - 24;
        int thumb_h = track_h * SET_VISIBLE_ROWS / VISIBLE_SETTINGS;
        int thumb_y = SET_VIEW_Y0 + 12 +
                      (int)((int64_t)track_h * st->set_scroll / max_top);
        if (thumb_y + thumb_h > SET_VIEW_Y0 + 12 + track_h)
            thumb_y = SET_VIEW_Y0 + 12 + track_h - thumb_h;
        ui_rect(s, 1852, SET_VIEW_Y0 + 12, 4, track_h, orbis_sep());
        ui_rect(s, 1852, thumb_y, 4, thumb_h, COL_DIM);
    }
}

/* --- on-screen keyboard (native panel) ----------------------------------------
 * Geometry is derived from the content (rows x cols), so the grid always
 * ends inside the panel and the panel inside the 1080p safe area - the
 * fixed 660/740 px panels of the first cut pushed the third key row past
 * the screen bottom and the footer over the keys. */

static void orbis_osk(ui_state_t *st, ui_surface_t *s) {
    ui_scrim(s, 0xFF000000, 110);

    const int is_mac = st->osk_target == SET_WOL_MAC;
    const int rows = st->osk_rows; /* 4 (IP) or 3 (hex) */
    const int cols = st->osk_cols; /* 3 (IP) or 6 (hex) */
    const int key = 94, hgap = 24, vgap = 14;
    const int pad_side = 56;

    int grid_w = cols * key + (cols - 1) * hgap;
    int grid_h = rows * key + (rows - 1) * vgap;
    /* worst-case field value keeps the panel wide enough to type blind */
    int field_w = ui_label_w(40, 0, is_mac ? "AA:BB:CC:DD:EE:FF"
                                           : "192.168.100.100") + 48;
    /* the footer row (Add/Delete/Cancel + Accept) must fit in one line:
     * the first cut sized the panel from the grid alone, which let the
     * right-anchored Accept group land on top of Cancel. */
    int footer_w = ui_ps_btn_w(30, PSBTN_CROSS) + 14 +
                   ui_label_w(22, 0, "Add") + 60 +
                   ui_ps_btn_w(30, PSBTN_SQUARE) + 14 +
                   ui_label_w(22, 0, "Delete") + 60 +
                   ui_ps_btn_w(30, PSBTN_CIRCLE) + 14 +
                   ui_label_w(22, 0, "Cancel") + 48 +
                   ui_ps_btn_w(30, PSBTN_OPTIONS) + 14 +
                   ui_label_w(22, 0, "Accept");
    int content_w = grid_w > field_w ? grid_w : field_w;
    if (footer_w > content_w)
        content_w = footer_w;
    if (content_w < 420)
        content_w = 420;
    const int pw = content_w + pad_side * 2;
    const int ph = 324 + grid_h; /* header/field/footer stack + grid */
    const int px = (1920 - pw) / 2, py = (1080 - ph) / 2;

    const ui_theme_t *t = ui_theme_current();
    ui_round_rect_alpha(s, px, py, pw, ph, 14, t->panel, 250);
    ui_frame(s, px, py, pw, ph, t->focus, 2);

    const char *title = is_mac ? "Wake-on-LAN MAC" : "PC IP address";
    ui_label(s, px + pad_side, py + 44, 32, 1, COL_TEXT, title);
    ui_label(s, px + pad_side, py + 92, 22, 0, COL_DIM,
             is_mac ? "12 hex digits - separators optional. Empty means off."
                    : "Enter the IPv4 address of your gaming PC.");

    /* Value field: large text over a hairline with a caret block. */
    int fy = py + 136;
    ui_rect(s, px + pad_side, fy + 56, pw - pad_side * 2, 2, orbis_sep());
    int vw = ui_label_w(40, 0, st->osk_buf);
    int max_vw = pw - pad_side * 2 - 48;
    if (vw > max_vw) {
        ui_label_fit(s, px + pad_side, fy, 40, 0, COL_TEXT, st->osk_buf,
                     max_vw);
        ui_rect(s, px + pw - pad_side - 10, fy + 4, 10, 44, COL_ACCENT);
    } else {
        ui_label(s, px + pad_side, fy, 40, 0, COL_TEXT, st->osk_buf);
        ui_rect(s, px + pad_side + vw + 8, fy + 4, 10, 44, COL_ACCENT);
    }

    /* Key grid, centered in the panel. */
    int gx0 = px + (pw - grid_w) / 2;
    int gy0 = py + 218;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int x = gx0 + c * (key + hgap);
            int y = gy0 + r * (key + vgap);
            int sel = r == st->osk_row && c == st->osk_col;
            if (sel) {
                /* ONE crisp rounded outline on a lifted key - the old
                 * ui_focus glow ring plus the sharp ui_frame stacked into
                 * the "double border" artifact. Concentric with the 10 px
                 * keycap radius (10 + 6 inset = 16). */
                ui_round_rect_alpha(s, x - 6, y - 6, key + 12, key + 12,
                                    16, blend_col(t->panel, t->focus, 36),
                                    210);
                ui_round_rect_outline_aa(s, x - 6, y - 6, key + 12,
                                         key + 12, 16, 3.0f, t->focus);
            } else {
                ui_round_rect_alpha(s, x, y, key, key, 10,
                                    blend_col(t->bg, t->panel, 96), 160);
            }
            char ch[2] = { osk_char_at(st, r, c), 0 };
            int cw = ui_label_w(34, 0, ch);
            /* Center each glyph on its own ink box (a '.' would ride
             * low if centered on the full row box, the way the console
             * OSK centers every keycap glyph). */
            int gy = y + (key - 44) / 2;
            int t, b;
            if (ui_glyph_ink_span(0, (unsigned char)ch[0], &t, &b)) {
                int ink_h = (b - t + 1) * 34 / 64;
                gy = y + (key - ink_h) / 2 - t * 34 / 64;
            }
            ui_label(s, x + (key - cw) / 2, gy, 34, 0,
                     sel ? COL_TEXT : COL_DIM, ch);
        }
    }

    /* Footer verbs inside the panel: the real chips, Add/Delete/Cancel
     * left, Accept (OPTIONS) right. */
    int hy = py + ph - 74;
    int hx = px + pad_side;
    hx = orbis_fn_at(s, hx, hy, "Add", PSBTN_CROSS, 1);
    hx = orbis_fn_at(s, hx, hy, "Delete", PSBTN_SQUARE, 1);
    hx = orbis_fn_at(s, hx, hy, "Cancel", PSBTN_CIRCLE, 1);
    {
        const char *lab = "Accept";
        int cw = ui_ps_btn_w(30, PSBTN_OPTIONS);
        int ax = px + pw - pad_side - cw - 14 - ui_label_w(22, 0, lab);
        orbis_fn_at(s, ax, hy, lab, PSBTN_OPTIONS, 1);
    }
}

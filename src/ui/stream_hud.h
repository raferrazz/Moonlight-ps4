/* In-stream HUD layer: quick menu, perf overlay, hold-progress ring and
 * the combo-hint pill, drawn over the presented game frame.
 *
 * Pure drawing over a caller-provided BGRA surface - no Orbis calls, no
 * timing state - so the same code renders on the PS4 video thread and in
 * the host harness (tests/host/render_hud.c). The renderer keeps owning
 * when/whether each element shows; this file owns how it looks. */
#pragma once

#include "ui_draw.h"
#include "quick_menu.h"
#include "../video/video.h" /* video_live_stats_t */

/* One hint entry: "KEYS  action". */
typedef struct {
    const char *keys;
    const char *action;
} stream_hint_t;

/* Quick menu panel (call only while qm.open). reveal_permille 0..1000
 * drives the left-edge slide-in (1000 = settled). */
void stream_hud_quick_menu(ui_surface_t *s, const qm_view_t *qm,
                           int reveal_permille);

/* Performance overlay panel. */
void stream_hud_stats(ui_surface_t *s, const video_live_stats_t *live);

/* One-time congestion warning pill (top center, amber); alpha lets the
 * caller fade it out. */
void stream_hud_warning(ui_surface_t *s, int alpha);

/* L3+R3 hold-progress ring; progress is 0..1000 permille. */
void stream_hud_hold(ui_surface_t *s, int progress_permille);

/* Bottom-center combo-hint pill; alpha lets the caller fade it out. */
void stream_hud_hint(ui_surface_t *s, const stream_hint_t *hints, int nhints,
                    int alpha);

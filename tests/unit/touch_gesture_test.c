/* touch_gesture.c unit tests: the touchpad-mouse state machine.
 * Recreated from the documented gesture rules (see touch_gesture.h):
 * one finger = pointer, two resting + click = right button, one finger +
 * click = left button, two-finger drag past the threshold = wheel, and
 * no button may ever stay stuck across mode transitions. */
#include "../../src/input/touch_gesture.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(const char *what, long got, long want) {
    if (got != want) {
        printf("FAIL: %s: got %ld want %ld\n", what, got, want);
        g_failed++;
    } else {
        printf("ok: %s\n", what);
    }
}

static touch_report_t rep(int fingers, int id0, int x0, int y0, int id1,
                          int x1, int y1, int click) {
    touch_report_t r;
    memset(&r, 0, sizeof(r));
    r.fingers = fingers;
    if (fingers > 0) { r.id[0] = id0; r.x[0] = x0; r.y[0] = y0; }
    if (fingers > 1) { r.id[1] = id1; r.x[1] = x1; r.y[1] = y1; }
    r.click = click;
    return r;
}

/* Feed one report and return the action count; actions land in a[] with
 * deltas in dx[]/dy[]. */
static int feed(touch_gesture_t *g, touch_report_t r, tg_action_t *a,
                int *dx, int *dy) {
    return tg_feed(g, &r, a, 8, dx, dy);
}

int main(void) {
    touch_gesture_t g;
    tg_action_t a[8];
    int dx[8], dy[8];
    int n;

    /* 1. Idle reports produce nothing. */
    tg_reset(&g);
    n = feed(&g, rep(0, 0, 0, 0, 0, 0, 0, 0), a, dx, dy);
    expect("idle: no actions", n, 0);
    expect("idle: mode none", g.mode, TG_NONE);

    /* 2. One finger landing anchors without a jump. */
    tg_reset(&g);
    n = feed(&g, rep(1, 7, 960, 475, 0, 0, 0, 0), a, dx, dy);
    expect("anchor: no move on landing", n, 0);

    /* 3. One finger dragging moves the pointer. */
    n = feed(&g, rep(1, 7, 1000, 475, 0, 0, 0, 0), a, dx, dy);
    expect("drag: one action", n, 1);
    expect("drag: is a move", a[0], TG_ACT_MOVE);
    expect("drag: dx +40", dx[0], 40);
    expect("drag: dy 0", dy[0], 0);

    /* 4. Negative deltas flow through. */
    n = feed(&g, rep(1, 7, 940, 500, 0, 0, 0, 0), a, dx, dy);
    expect("drag back: dx -60", dx[0], -60);
    expect("drag down: dy +25", dy[0], 25);

    /* 5. One finger + physical click = left button. */
    n = feed(&g, rep(1, 7, 940, 500, 0, 0, 0, 1), a, dx, dy);
    expect("click: left down emitted",
           n >= 1 && a[n - 1] == TG_ACT_L_DOWN, 1);
    n = feed(&g, rep(1, 7, 940, 500, 0, 0, 0, 0), a, dx, dy);
    expect("click release: left up emitted",
           n >= 1 && a[n - 1] == TG_ACT_L_UP, 1);

    /* 6. Second finger lands: pointer flushes, mode goes pending, and a
     * click in pending mode is a RIGHT button. */
    n = feed(&g, rep(2, 7, 940, 500, 9, 1000, 500, 0), a, dx, dy);
    expect("second finger: pending mode", g.mode, TG_PENDING);
    n = feed(&g, rep(2, 7, 940, 500, 9, 1000, 500, 1), a, dx, dy);
    expect("two-finger click: right down",
           n >= 1 && a[n - 1] == TG_ACT_R_DOWN, 1);
    n = feed(&g, rep(2, 7, 940, 500, 9, 1000, 500, 0), a, dx, dy);
    expect("two-finger release: right up",
           n >= 1 && a[n - 1] == TG_ACT_R_UP, 1);

    /* 7. Small pair movement under the threshold stays pending (no
     * scroll, so a two-finger click never scrolls). */
    n = feed(&g, rep(2, 7, 950, 500, 9, 1010, 500, 0), a, dx, dy);
    expect("small pair move: still pending", g.mode, TG_PENDING);
    expect("small pair move: no scroll emitted", n, 0);

    /* 8. Dragging the pair past the threshold locks scrolling. */
    touch_report_t r = rep(2, 7, 950 + 40, 540, 9, 1010 + 40, 540, 0);
    n = feed(&g, r, a, dx, dy);
    expect("threshold cross: scroll mode", g.mode, TG_SCROLL);
    (void)n;
    /* Travel 40 >= TG_SCROLL_LOCK_PX; wheel flushes once a notch of 128
     * px accumulates. */
    r = rep(2, 7, 950 + 40 + 64, 540 + 64, 9, 1010 + 40 + 64, 540 + 64, 0);
    n = feed(&g, r, a, dx, dy);
    int saw_scroll = 0, scroll_dy = 0;
    for (int i = 0; i < n; i++)
        if (a[i] == TG_ACT_SCROLL) {
            saw_scroll = 1;
            if (dy[i] != 0) { /* vertical is emitted first; keep it */ 
                if (scroll_dy == 0)
                    scroll_dy = dy[i];
            }
        }
    expect("wheel: scroll emitted once past the notch", saw_scroll, 1);
    /* High-res wheel: partial ticks are emitted (fraction of 120), and
     * the remainder stays accumulated for the next flush. */
    expect("wheel: positive partial tick", scroll_dy > 0, 1);
    expect("wheel: at most one notch per flush", scroll_dy <= TG_WHEEL_UNIT, 1);

    /* 9. A click during a scroll never presses a button (noise guard). */
    r = rep(2, 7, 990 + 40 + 64, 604 + 64, 9, 1050 + 40 + 64, 604 + 64, 1);
    n = feed(&g, r, a, dx, dy);
    int saw_click = 0;
    for (int i = 0; i < n; i++)
        if (a[i] == TG_ACT_L_DOWN || a[i] == TG_ACT_R_DOWN)
            saw_click = 1;
    expect("click during scroll: no button", saw_click, 0);

    /* 10. Losing a finger flushes the scroll and releases buttons. */
    n = feed(&g, rep(1, 7, 990 + 40 + 64, 604 + 64, 0, 0, 0, 0), a, dx, dy);
    expect("finger lost: back to pointer mode", g.mode, TG_POINTER);

    /* 11. Everything lifted: buttons released, mode none. */
    n = feed(&g, rep(0, 0, 0, 0, 0, 0, 0, 0), a, dx, dy);
    expect("all lifted: mode none", g.mode, TG_NONE);
    expect("all lifted: no stuck buttons", g.left + g.right, 0);

    /* 12. A right click held while the pair turns into a scroll gets
     * force-released (no stuck press on the host). */
    tg_reset(&g);
    feed(&g, rep(2, 3, 500, 400, 4, 700, 400, 1), a, dx, dy); /* right down */
    expect("right pressed in pending", g.right, 1);
    feed(&g, rep(2, 3, 540, 440, 4, 740, 440, 1), a, dx, dy); /* cross lock */
    feed(&g, rep(2, 3, 640, 540, 4, 840, 540, 1), a, dx, dy); /* keep scroll */
    expect("scroll guard: right released", g.right, 0);

    /* 13. Same finger id re-landing re-anchors (no teleport deltas). */
    tg_reset(&g);
    feed(&g, rep(1, 5, 100, 100, 0, 0, 0, 0), a, dx, dy);
    n = feed(&g, rep(0, 0, 0, 0, 0, 0, 0, 0), a, dx, dy); /* lift */
    n = feed(&g, rep(1, 5, 900, 900, 0, 0, 0, 0), a, dx, dy); /* land far */
    expect("re-anchor: no phantom move", n, 0);

    /* 14. Diagonal scroll keeps both axes accumulating. */
    tg_reset(&g);
    feed(&g, rep(2, 1, 100, 100, 2, 200, 100, 0), a, dx, dy);
    feed(&g, rep(2, 1, 180, 180, 2, 280, 180, 0), a, dx, dy); /* lock */
    feed(&g, rep(2, 1, 300, 300, 2, 400, 300, 0), a, dx, dy);
    feed(&g, rep(2, 1, 300 + 128, 300 + 128, 2, 400 + 128, 300 + 128, 0),
         a, dx, dy);
    expect("diagonal scroll: still scrolling", g.mode, TG_SCROLL);
    int scroll_axes = 0;
    feed(&g, rep(2, 1, 300 + 128 + 64, 300 + 128 + 64, 2, 400 + 128 + 64,
                 300 + 128 + 64, 0), a, dx, dy);
    n = feed(&g, rep(2, 1, 300 + 128 + 128, 300 + 128 + 128, 2,
                     400 + 128 + 128, 300 + 128 + 128, 0), a, dx, dy);
    for (int i = 0; i < n; i++)
        if (a[i] == TG_ACT_SCROLL)
            scroll_axes++;
    expect("diagonal scroll: axes emit wheel deltas", scroll_axes > 0, 1);

    if (g_failed) {
        printf("== %d touch test(s) failed ==\n", g_failed);
        return 1;
    }
    printf("== all touch_gesture tests passed ==\n");
    return 0;
}

/* quick_menu.c unit tests — the pure state machine the stream loop drives.
 * Recreated from the semantics documented in quick_menu.h: navigation wrap,
 * bitrate stepping + clamps, the seed-based change flag (step up and back
 * must NOT reconnect), toggle rows and quit actions, close semantics. */
#include "../../src/ui/quick_menu.h"

#include <stdio.h>

static int g_failed;

static void expect(const char *what, long got, long want) {
    if (got != want) {
        printf("FAIL: %s: got %ld want %ld\n", what, got, want);
        g_failed++;
    } else {
        printf("ok: %s\n", what);
    }
}

static int act_of(qm_action_t a) { return (int)a; }

int main(void) {
    /* 1. open resets selection and seeds values. */
    qm_open(1, 0, 20000);
    qm_view_t v;
    qm_view(&v);
    expect("open: open=1", v.open, 1);
    expect("open: sel starts at first row", v.sel, QM_ROW_TOUCHPAD);
    expect("open: touchpad state seeded", v.touchpad, 1);
    expect("open: overlay state seeded", v.overlay, 0);
    expect("open: bitrate seeded", v.bitrate, 20000);
    expect("open: nothing changed yet", v.bitrate_changed, 0);

    /* 2. is_open. */
    expect("is_open while open", qm_is_open(), 1);

    /* 3. DOWN wraps through all five rows. */
    qm_input(MENU_BTN_DOWN); qm_view(&v);
    expect("DOWN -> overlay row", v.sel, QM_ROW_OVERLAY);
    qm_input(MENU_BTN_DOWN); qm_input(MENU_BTN_DOWN); qm_input(MENU_BTN_DOWN);
    qm_view(&v);
    expect("3x DOWN -> quit-app row", v.sel, QM_ROW_QUIT_APP);
    qm_input(MENU_BTN_DOWN); qm_view(&v);
    expect("DOWN wraps to first row", v.sel, QM_ROW_TOUCHPAD);

    /* 4. UP wraps backwards. */
    qm_input(MENU_BTN_UP); qm_view(&v);
    expect("UP wraps to last row", v.sel, QM_ROW_QUIT_APP);

    /* 5. Bitrate row: LEFT/RIGHT step by 1000 (fine) within bounds. */
    qm_open(0, 0, 20000);
    for (int i = 0; i < 2; i++)
        qm_input(MENU_BTN_DOWN); /* now on bitrate row (index 2) */
    qm_view(&v);
    expect("nav: bitrate row reached", v.sel, QM_ROW_BITRATE);
    qm_input(MENU_BTN_RIGHT); qm_view(&v);
    expect("RIGHT +1000", v.bitrate, 21000);
    qm_input(MENU_BTN_LEFT); qm_input(MENU_BTN_LEFT); qm_view(&v);
    expect("2x LEFT -2000", v.bitrate, 19000);

    /* 5b. L1/R1 take the big step (Settings-row increments). */
    qm_input(MENU_BTN_R1); qm_view(&v);
    expect("R1 +5000", v.bitrate, 24000);
    qm_input(MENU_BTN_L1); qm_view(&v);
    expect("L1 -5000", v.bitrate, 19000);

    /* 6. Clamps at both ends. */
    qm_open(0, 0, 6000);
    for (int i = 0; i < 2; i++)
        qm_input(MENU_BTN_DOWN);
    qm_input(MENU_BTN_LEFT); qm_view(&v);
    expect("clamp low = 5000", v.bitrate, 5000);
    qm_input(MENU_BTN_L1); qm_view(&v);
    expect("clamp low holds (big step too)", v.bitrate, 5000);
    qm_open(0, 0, 49000);
    for (int i = 0; i < 2; i++)
        qm_input(MENU_BTN_DOWN);
    qm_input(MENU_BTN_RIGHT); qm_view(&v);
    expect("clamp high = 50000", v.bitrate, 50000);
    qm_input(MENU_BTN_RIGHT); qm_view(&v);
    expect("clamp high holds", v.bitrate, 50000);

    /* 7. CROSS on the bitrate row steps up by the fine step. */
    qm_open(0, 0, 10000);
    for (int i = 0; i < 2; i++)
        qm_input(MENU_BTN_DOWN);
    qm_input(MENU_BTN_CROSS); qm_view(&v);
    expect("CROSS on bitrate steps up", v.bitrate, 11000);

    /* 8. Step up and back: change flag must stay clear (no reconnect). */
    qm_open(0, 0, 20000);
    for (int i = 0; i < 2; i++)
        qm_input(MENU_BTN_DOWN);
    qm_input(MENU_BTN_RIGHT); qm_input(MENU_BTN_LEFT); qm_view(&v);
    expect("up+back: value restored", v.bitrate, 20000);
    expect("up+back: not changed (no reconnect)", v.bitrate_changed, 0);
    qm_input(MENU_BTN_RIGHT); qm_view(&v);
    expect("real change flagged", v.bitrate_changed, 1);

    /* 9. Toggle rows flip their state and report their action. */
    qm_open(1, 0, 20000);
    expect("X on touchpad row toggles",
           act_of(qm_input(MENU_BTN_CROSS)), (int)QM_ACT_TOGGLE_TOUCHPAD);
    qm_view(&v);
    expect("touchpad now off", v.touchpad, 0);
    qm_input(MENU_BTN_DOWN);
    expect("X on overlay row toggles",
           act_of(qm_input(MENU_BTN_CROSS)), (int)QM_ACT_TOGGLE_OVERLAY);
    qm_view(&v);
    expect("overlay now on", v.overlay, 1);
    expect("overlay toggle did not touch touchpad", v.touchpad, 0);

    /* 10. Quit rows report their actions without changing toggles. */
    qm_input(MENU_BTN_DOWN); /* bitrate row */
    qm_input(MENU_BTN_DOWN); /* quit stream */
    expect("X on quit stream", act_of(qm_input(MENU_BTN_CROSS)),
           (int)QM_ACT_QUIT);
    qm_input(MENU_BTN_DOWN); /* quit app */
    expect("X on quit app on PC", act_of(qm_input(MENU_BTN_CROSS)),
           (int)QM_ACT_QUIT_APP);
    qm_view(&v);
    expect("quit rows leave toggles alone", v.overlay, 1);

    /* 11. CIRCLE requests close (no other action). */
    expect("CIRCLE closes", act_of(qm_input(MENU_BTN_CIRCLE)),
           (int)QM_ACT_CLOSE);

    /* 12. close(): hands back the live value + the change verdict. */
    qm_open(0, 0, 20000);
    for (int i = 0; i < 2; i++)
        qm_input(MENU_BTN_DOWN);
    qm_input(MENU_BTN_RIGHT);
    int bout = 0, changed = 0;
    qm_close(&bout, &changed);
    expect("close: bitrate out", bout, 21000);
    expect("close: changed verdict", changed, 1);
    expect("close: no longer open", qm_is_open(), 0);

    /* 13. close when never changed -> changed=0. */
    qm_open(1, 1, 30000);
    qm_close(&bout, &changed);
    expect("close: untouched value out", bout, 30000);
    expect("close: untouched not changed", changed, 0);

    /* 14. Input on a closed menu is inert. */
    expect("input while closed: none",
           act_of(qm_input(MENU_BTN_CROSS)), (int)QM_ACT_NONE);
    expect("empty input: none", act_of(qm_input(0)), (int)QM_ACT_NONE);

    /* 15. close() is safe when already closed. */
    qm_close(NULL, NULL);
    expect("double close safe", 1, 1);

    /* 16. Row inventory: the menu stays the five rows below - a stream
     * restart must never reappear as a quick-menu action (it reconnects
     * blindly from inside the stream loop; the launch screen's ladder
     * already owns recovery). Locks the row count AND the full action set
     * a row's CROSS can produce. */
    expect("row count stays 5", QM_ROW_COUNT, 5);
    expect("no sixth row id", QM_ROW_QUIT_APP + 1, QM_ROW_COUNT);
    {
        qm_action_t acts[QM_ROW_COUNT];
        for (int r = 0; r < QM_ROW_COUNT; r++) {
            qm_open(1, 1, 20000);
            for (int i = 0; i < r; i++)
                qm_input(MENU_BTN_DOWN);
            acts[r] = qm_input(MENU_BTN_CROSS);
            qm_close(NULL, NULL);
        }
        int legal = 1;
        for (int r = 0; r < QM_ROW_COUNT; r++) {
            qm_action_t a = acts[r];
            if (a != QM_ACT_NONE && a != QM_ACT_TOGGLE_TOUCHPAD &&
                a != QM_ACT_TOGGLE_OVERLAY && a != QM_ACT_QUIT &&
                a != QM_ACT_QUIT_APP) {
                legal = 0;
                printf("FAIL: row %d returned action %d\n", r, (int)a);
            }
        }
        expect("every row action is in the known set", legal, 1);
    }

    if (g_failed) {
        printf("== %d quick_menu test(s) failed ==\n", g_failed);
        return 1;
    }
    printf("== all quick_menu tests passed ==\n");
    return 0;
}

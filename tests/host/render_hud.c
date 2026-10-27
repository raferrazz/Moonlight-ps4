/* Renders the in-stream HUD (ui/stream_hud.c) over a synthetic game frame
 * on the host, for before/after review of the quick menu, perf overlay,
 * hold ring and hint pill. */
#include "../../src/ui/ui_draw.h"
#include "../../src/ui/stream_hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1920
#define H 1080

static void paint_game(uint8_t *fb) {
    /* Stand-in busy game frame: warm gradient + light shafts so overlay
     * legibility is honestly judged. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = fb + ((size_t)y * W + x) * 4;
            float t = y / (float)H;
            p[0] = (uint8_t)(28 + t * 90);      /* B */
            p[1] = (uint8_t)(30 + t * 60);      /* G */
            p[2] = (uint8_t)(38 + (1 - t) * 120); /* R */
            p[3] = 255;
        }
    }
    ui_surface_t s = { fb, W * 4, W, H };
    for (int k = 0; k < 6; k++) {
        int cx = 180 + k * 300, cy = 300 + (k % 3) * 180;
        ui_circle(&s, cx, cy, 90, 0xff5a4a3a);
        ui_circle(&s, cx, cy, 70, 0xff7a5a3f);
    }
}

static void save(const char *name, uint8_t *fb) {
    char path[256];
    const char *dir = getenv("HUD_SHOTS") ? getenv("HUD_SHOTS") : ".";
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        const uint8_t *row = fb + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) {
            uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("hud capture: %s\n", name);
}

int main(void) {
    uint8_t *fb = malloc((size_t)W * H * 4);
    if (!fb)
        return 1;
    ui_surface_t s = { fb, W * 4, W, H };

    const stream_hint_t hints[2] = {
        { "L1 + R1 + OPTIONS", "PS guide" },
        { "L3 + R3", "Hold for Stream menu" },
    };

    /* hint pill */
    paint_game(fb);
    stream_hud_hint(&s, hints, 2, 168);
    save("hud-hint", fb);

    /* hold ring mid-progress */
    paint_game(fb);
    stream_hud_hold(&s, 640);
    save("hud-hold", fb);

    /* perf overlay with a realistic window */
    paint_game(fb);
    video_live_stats_t live;
    memset(&live, 0, sizeof(live));
    live.fps = 59.4f;
    live.decode_ms = 6.2f;
    live.convert_ms = 3.1f;
    live.present_ms = 1.9f;
    live.kb_per_frame = 39.0f;
    live.net_recv_ms = 1.2f;
    live.host_ms = 4.5f;
    live.rx_mbps = 18.6f;
    live.target_fps = 60;
    live.dropped_window = 3;
    live.frame_count = 120;
    for (int i = 0; i < 120; i++) {
        live.frame_ms[i] = 16.7f;
        if (i % 17 == 0) live.frame_ms[i] = 21.3f;
        if (i % 41 == 0) live.frame_ms[i] = 34.6f;
        if (i % 7 == 0) live.frame_ms[i] = 15.9f;
    }
    stream_hud_stats(&s, &live);
    save("hud-stats", fb);

    /* 30 fps session: the budget line, bar colors and the verdict must
     * follow the target, not a hardcoded 60. */
    paint_game(fb);
    live.fps = 29.9f;
    live.target_fps = 30;
    live.dropped_window = 0;
    live.frame_count = 120;
    for (int i = 0; i < 120; i++) {
        live.frame_ms[i] = 33.4f;
        if (i % 23 == 0) live.frame_ms[i] = 41.0f;
    }
    stream_hud_stats(&s, &live);
    save("hud-stats-30fps", fb);

    /* congestion warning pill (the one-time amber state) */
    paint_game(fb);
    stream_hud_warning(&s, 220);
    save("hud-warning", fb);

    /* quick menu on the bitrate row */
    paint_game(fb);
    qm_view_t qm;
    memset(&qm, 0, sizeof(qm));
    qm.open = 1; qm.sel = QM_ROW_BITRATE; qm.touchpad = 1; qm.overlay = 0;
    qm.bitrate = 25000; qm.bitrate_changed = 1;
    stream_hud_quick_menu(&s, &qm, 1000);
    save("hud-quickmenu-bitrate", fb);

    /* quick menu on a toggle row */
    paint_game(fb);
    qm.sel = QM_ROW_TOUCHPAD; qm.bitrate = 20000; qm.bitrate_changed = 0;
    stream_hud_quick_menu(&s, &qm, 1000);
    save("hud-quickmenu-touchpad", fb);

    /* quick menu mid-slide */
    paint_game(fb);
    qm.sel = QM_ROW_QUIT; qm.overlay = 0;
    stream_hud_quick_menu(&s, &qm, 380);
    save("hud-quickmenu-slide", fb);

    /* quit-row content card */
    paint_game(fb);
    qm.sel = QM_ROW_QUIT_APP;
    stream_hud_quick_menu(&s, &qm, 1000);
    save("hud-quickmenu-quitapp", fb);

    /* everything at once (how a session can actually look) */
    paint_game(fb);
    stream_hud_stats(&s, &live);
    stream_hud_hold(&s, 320);
    stream_hud_hint(&s, hints, 2, 168);
    qm.sel = QM_ROW_OVERLAY; qm.overlay = 1;
    stream_hud_quick_menu(&s, &qm, 1000);
    save("hud-all", fb);

    free(fb);
    return 0;
}

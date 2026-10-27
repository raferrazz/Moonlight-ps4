/* Scenario driver: runs the REAL menu code (ui_menu.c) on the host against
 * scripted input and a deterministic gamestream stub, capturing presented
 * frames for before/after review and asserting the menu's return codes.
 *
 * Scenarios are lists of timed steps executed by a director thread while
 * the menu loop runs on the main thread:
 *   CAPTURE <name>     save the last presented frame as <name>.ppm
 *   PRESS <mask>       queue a button edge
 *   NOPAD <0|1>         simulate pad disconnect/reconnect
 *   GS <mode>           set refresh-stub behavior
 *
 * Exit code is non-zero when any asserted menu result mismatches. */
#include "host_harness.h"

#include "../../src/config.h"
#include "../../src/ui/ui_menu.h"
#include "../../src/ui/ui_theme.h"
#include "../../src/gamestream/client.h"
#include "../../src/gamestream/discovery.h"
#include "../../src/input/input_pad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* scenario engine                                                      */
/* ------------------------------------------------------------------ */

enum { OP_SLEEP, OP_CAPTURE, OP_PRESS, OP_NOPAD, OP_GS };

typedef struct {
    int at_ms;
    int op;
    unsigned arg;
    const char *name;
} step_t;

typedef struct {
    const step_t *steps;
    int nsteps;
    long t0;
    pthread_mutex_t start_mtx;
} scenario_t;

static void *director(void *arg) {
    scenario_t *sc = arg;
    pthread_mutex_lock(&sc->start_mtx); /* wait until menu loop is running */
    pthread_mutex_unlock(&sc->start_mtx);
    for (int i = 0; i < sc->nsteps; i++) {
        const step_t *s = &sc->steps[i];
        long target = s->at_ms; /* relative to t0, like `now` below */
        long now;
        struct timespec ts;
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L - sc->t0;
            if (now >= target)
                break;
            usleep((useconds_t)((target - now) * 1000));
        }
        switch (s->op) {
        case OP_CAPTURE:
            harness_capture(s->name);
            break;
        case OP_PRESS:
            harness_press(s->arg, 0);
            break;
        case OP_NOPAD:
            harness_set_no_pad((int)s->arg);
            break;
        case OP_GS:
            harness_gs_refresh((int)s->arg);
            break;
        default:
            break;
        }
    }
    return NULL;
}

static int run_menu_with_steps(app_config_t *cfg, gs_server_t *server,
                               const char *dir, const step_t *steps,
                               int nsteps) {
    harness_clear_input();
    harness_reset_frame_count();
    scenario_t sc;
    sc.steps = steps;
    sc.nsteps = nsteps;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sc.t0 = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    pthread_mutex_init(&sc.start_mtx, NULL);
    pthread_mutex_lock(&sc.start_mtx);
    pthread_t th;
    pthread_create(&th, NULL, director, &sc);
    pthread_mutex_unlock(&sc.start_mtx); /* released immediately: menu start
                                          * takes ~100 ms at most */
    int rc = ui_menu_run(cfg, server, dir);
    pthread_join(th, NULL);
    pthread_mutex_destroy(&sc.start_mtx);
    return rc;
}

/* Blocking-call wrapper: runs `fn(arg)` on a runner thread while the
 * director executes capture/press steps on the main thread. */
typedef struct {
    void (*fn)(void *arg);
    void *arg;
} runner_t;

static void *runner_thread(void *p) {
    runner_t *r = p;
    r->fn(r->arg);
    return NULL;
}

static void run_blocking_with_steps(void (*fn)(void *), void *arg,
                                   const step_t *steps, int nsteps) {
    harness_clear_input();
    harness_reset_frame_count();
    scenario_t sc;
    sc.steps = steps;
    sc.nsteps = nsteps;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sc.t0 = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    runner_t r = { fn, arg };
    pthread_t runner, dir;
    pthread_mutex_init(&sc.start_mtx, NULL);
    pthread_create(&runner, NULL, runner_thread, &r);
    usleep(150 * 1000); /* let the screen come up */
    pthread_mutex_lock(&sc.start_mtx);
    pthread_create(&dir, NULL, director, &sc);
    pthread_mutex_unlock(&sc.start_mtx);
    pthread_join(dir, NULL);
    pthread_join(runner, NULL);
    pthread_mutex_destroy(&sc.start_mtx);
}

static int g_failures;

static void check(const char *what, int got, int want) {
    if (got != want) {
        printf("ASSERT FAIL: %s: got %d want %d\n", what, got, want);
        g_failures++;
    } else {
        printf("assert ok: %s => %d\n", what, got);
    }
}

/* ------------------------------------------------------------------ */
/* fixtures                                                             */
/* ------------------------------------------------------------------ */

#define FIXDIR "build-tests/fixture"

static char g_identity[64]; /* dummy client_identity_t backing store */

static void make_server(gs_server_t *server, const char *address, int paired,
                        int current_game) {
    memset(server, 0, sizeof(*server));
    snprintf(server->address, sizeof(server->address), "%s", address);
    server->httpPort = 47989;
    server->httpsPort = 47990;
    server->paired = paired != 0;
    server->currentGame = current_game;
    server->id = (client_identity_t *)g_identity;
    snprintf(server->appVersion, sizeof(server->appVersion), "Sunshine 7.1.431");
    snprintf(server->host_mac, sizeof(server->host_mac), "3C:97:0E:12:34:56");
}

static void write_cache(const char *dir, int long_list) {
    char path[512];
    snprintf(path, sizeof(path), "%s/applist.cache", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "host %s\n", "127.0.0.1");
    if (long_list) {
        static const char *names[] = {
            "Asteroid Miners", "Blue Harbor", "Cinderfall Tactics",
            "Dust Circuit", "Ember Voyage", "Frostline Rally",
            "Gilded Depths", "Hollow Meridian", "Ivory Peaks",
            "Juniper Sky", "Kestrel Run", "Lumen Drift", "Marrow Fen",
            "Nimbus Gate",
        };
        for (int i = 0; i < 14; i++)
            fprintf(f, "%d\t%s\n", 2000 + i, names[i]);
        fprintf(f, "9999\tDesktop\n");
    } else {
        static const char *names[] = {
            "Asteroid Miners", "Blue Harbor", "Cinderfall Tactics",
            "Dust Circuit", "Ember Voyage", "Frostline Rally",
            "Gilded Depths", "Hollow Meridian", "Ivory Peaks", "Desktop",
        };
        for (int i = 0; i < 9; i++)
            fprintf(f, "%d\t%s\n", 1001 + i, names[i]);
        fprintf(f, "9999\tDesktop\n");
    }
    fclose(f);
}

static void base_cfg(app_config_t *cfg, int theme) {
    config_set_defaults(cfg);
    snprintf(cfg->host, sizeof(cfg->host), "%s", "127.0.0.1");
    cfg->http_port = 47989;
    snprintf(cfg->app_name, sizeof(cfg->app_name), "%s", "1003");
    cfg->ui_theme = theme;
    cfg->stream.width = 1920;
    cfg->stream.height = 1080;
    cfg->stream.fps = 60;
    cfg->stream.bitrate = 20000;
    snprintf(cfg->wol_mac, sizeof(cfg->wol_mac), "%s", "3C:97:0E:12:34:56");
    cfg->wol_auto = true;
}

/* ------------------------------------------------------------------ */
/* scenarios                                                            */
/* ------------------------------------------------------------------ */

/* Per-theme tour: games -> settings -> pcs -> games, then exit via the
 * Settings -> Exit row. */
static void scenario_theme_tour(int theme) {
    char tag[16];
    const char *theme_name = ui_theme_name(theme);
    snprintf(tag, sizeof(tag), "%d", theme);
    char name_games[64], name_settings[64], name_pcs[64];
    snprintf(name_games, sizeof(name_games), "t%d-games", theme);
    snprintf(name_settings, sizeof(name_settings), "t%d-settings", theme);
    snprintf(name_pcs, sizeof(name_pcs), "t%d-pcs", theme);

    static step_t steps[] = {
        { 1300, OP_CAPTURE, 0, "" }, { 1600, OP_PRESS, MENU_BTN_R1, "" },
        { 2200, OP_CAPTURE, 0, "" }, { 2500, OP_PRESS, MENU_BTN_R1, "" },
        { 3100, OP_CAPTURE, 0, "" }, { 3400, OP_PRESS, MENU_BTN_R1, "" },
        { 4000, OP_CAPTURE, 0, "" }, { 4300, OP_PRESS, MENU_BTN_R1, "" },
        /* walk to the Exit row and press X (15 rows, sel starts at 0) */
        { 4500, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4530, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4560, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4590, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4620, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4650, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4680, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4710, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4740, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4770, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4800, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4830, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4860, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4890, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4950, OP_PRESS, MENU_BTN_CROSS, "" },
        { 5600, OP_SLEEP, 0, "" },
    };
    steps[0].name = name_games;
    steps[2].name = name_settings;
    steps[4].name = name_pcs;
    steps[6].name = name_games; /* second pass: settled tab */

    char label[96];
    snprintf(label, sizeof(label), "theme %d (%s) menu exit", theme, theme_name);
    app_config_t cfg;
    base_cfg(&cfg, theme);
    gs_server_t server;
    make_server(&server, "127.0.0.1", 1, 1003);
    write_cache(FIXDIR, 0);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, &server, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    check(label, rc, UI_MENU_EXIT);
    (void)tag;
}

/* OSK flow: enter the Settings tab, open the PC IP keyboard, type, accept
 * an invalid value (notice must keep it open), cancel, exit. */
static void scenario_osk(void) {
    static step_t steps[] = {
        { 1500, OP_CAPTURE, 0, "osk-0-games" },
        { 1700, OP_PRESS, MENU_BTN_R1, "" },
        { 2300, OP_CAPTURE, 0, "osk-1-settings" },
        { 2400, OP_PRESS, MENU_BTN_CROSS, "" }, /* open IP keyboard */
        { 3000, OP_CAPTURE, 0, "osk-2-ip" },
        { 3100, OP_PRESS, MENU_BTN_CROSS, "" },  /* '1' */
        { 3200, OP_PRESS, MENU_BTN_RIGHT, "" },
        { 3300, OP_PRESS, MENU_BTN_CROSS, "" },  /* '2' */
        { 3400, OP_PRESS, MENU_BTN_DOWN, "" },
        { 3500, OP_PRESS, MENU_BTN_CROSS, "" },  /* '4' */
        { 3900, OP_CAPTURE, 0, "osk-3-typed" },
        { 4000, OP_PRESS, MENU_BTN_OPTIONS, "" }, /* accept invalid -> notice */
        { 4600, OP_CAPTURE, 0, "osk-4-invalid" },
        { 4700, OP_PRESS, MENU_BTN_CIRCLE, "" },  /* cancel */
        { 5200, OP_CAPTURE, 0, "osk-5-closed" },
        { 5300, OP_PRESS, MENU_BTN_DOWN, "" },   /* WOL row: open MAC keyboard */
        { 5400, OP_PRESS, MENU_BTN_CROSS, "" },
        { 6000, OP_CAPTURE, 0, "osk-6-mac" },
        { 6100, OP_PRESS, MENU_BTN_CIRCLE, "" },
        { 6500, OP_PRESS, MENU_BTN_DOWN, "" }, /* move to Auto-start */
        { 6550, OP_PRESS, MENU_BTN_DOWN, "" }, /* Theme */
        { 6600, OP_CAPTURE, 0, "osk-7-theme-row" },
        { 6650, OP_PRESS, MENU_BTN_RIGHT, "" }, /* cycle theme */
        { 7200, OP_CAPTURE, 0, "osk-8-theme-cycled" },
        { 7250, OP_PRESS, MENU_BTN_LEFT, "" },   /* cycle back (parity of value) */
        /* walk down to Exit and leave */
        { 7500, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7530, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7560, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7590, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7620, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7650, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7680, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7710, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7740, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7770, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7800, OP_PRESS, MENU_BTN_DOWN, "" },
        { 7830, OP_PRESS, MENU_BTN_CROSS, "" },
        { 8400, OP_SLEEP, 0, "" },
    };
    app_config_t cfg;
    base_cfg(&cfg, 5);
    gs_server_t server;
    make_server(&server, "127.0.0.1", 1, 1003);
    write_cache(FIXDIR, 0);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, &server, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    check("osk scenario menu exit", rc, UI_MENU_EXIT);
}

/* Games grid navigation + launch: move the selection, check the Play
 * button state, launch (result must be UI_MENU_LAUNCH). */
static void scenario_games_launch(void) {
    static step_t steps[] = {
        { 1400, OP_CAPTURE, 0, "games-0-hero" },
        { 1600, OP_PRESS, MENU_BTN_RIGHT, "" },
        { 2100, OP_CAPTURE, 0, "games-1-moved" },
        { 2300, OP_PRESS, MENU_BTN_LEFT, "" },
        { 2800, OP_CAPTURE, 0, "games-2-back" },
        { 3000, OP_PRESS, MENU_BTN_CROSS, "" }, /* launch the selection */
        { 3600, OP_SLEEP, 0, "" },
    };
    app_config_t cfg;
    base_cfg(&cfg, 5);
    gs_server_t server;
    make_server(&server, "127.0.0.1", 1, 1003);
    write_cache(FIXDIR, 0);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, &server, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    check("games X => UI_MENU_LAUNCH", rc, UI_MENU_LAUNCH);
}

/* Long list (14 games): focus travel, position counter, wrap. */
static void scenario_games_long(void) {
    static step_t steps[] = {
        { 1400, OP_CAPTURE, 0, "long-0" },
        { 1600, OP_PRESS, MENU_BTN_RIGHT, "" },
        { 1650, OP_PRESS, MENU_BTN_RIGHT, "" },
        { 1700, OP_PRESS, MENU_BTN_RIGHT, "" },
        { 1750, OP_PRESS, MENU_BTN_RIGHT, "" },
        { 2200, OP_CAPTURE, 0, "long-1" },
        { 2400, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2850, OP_CAPTURE, 0, "long-2" },
        { 3000, OP_PRESS, MENU_BTN_SQUARE, "" }, /* mark default app */
        { 3500, OP_CAPTURE, 0, "long-3-default" },
        { 3700, OP_PRESS, MENU_BTN_SQUARE, "" },  /* clear it again */
        { 3900, OP_CAPTURE, 0, "long-4" },
        /* leave via Settings -> Exit */
        { 4200, OP_PRESS, MENU_BTN_R1, "" },
        { 4400, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4430, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4460, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4490, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4520, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4550, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4580, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4610, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4640, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4670, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4700, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4730, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4760, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4790, OP_PRESS, MENU_BTN_DOWN, "" },
        { 4880, OP_PRESS, MENU_BTN_CROSS, "" },
        { 5200, OP_SLEEP, 0, "" },
    };
    app_config_t cfg;
    base_cfg(&cfg, 5);
    gs_server_t server;
    make_server(&server, "127.0.0.1", 1, 1003);
    write_cache(FIXDIR, 1);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, &server, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    check("long grid exit", rc, UI_MENU_EXIT);
}

/* No server: PCs tab opens, saved host is seeded, scan verdict lands as
 * Offline; SQUARE sends WoL; X must return UI_MENU_RECONNECT. */
static void scenario_pcs_offline(void) {
    static step_t steps[] = {
        { 1500, OP_CAPTURE, 0, "pcs-0-offline" },
        { 1700, OP_PRESS, MENU_BTN_SQUARE, "" }, /* WoL burst */
        { 2400, OP_CAPTURE, 0, "pcs-1-wol" },
        { 2600, OP_PRESS, MENU_BTN_CROSS, "" },  /* connect */
        { 3200, OP_SLEEP, 0, "" },
    };
    app_config_t cfg;
    base_cfg(&cfg, 5);
    gs_server_t server;
    make_server(&server, "127.0.0.1", 0, 0);
    write_cache(FIXDIR, 0);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, NULL, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    (void)server;
    check("pcs X (no server, saved host) => UI_MENU_RECONNECT", rc,
          UI_MENU_RECONNECT);
}

/* No host, no server: empty PCs state (Add PC row only), the in-place
 * Add keyboard on the PCs tab, and the Games tab error state. */
static void scenario_first_boot(void) {
    static step_t steps[] = {
        { 1500, OP_CAPTURE, 0, "first-0-pcs-empty" },
        { 1700, OP_PRESS, MENU_BTN_CROSS, "" }, /* opens the Add PC keyboard */
        { 2400, OP_CAPTURE, 0, "first-1-osk" },
        { 2500, OP_PRESS, MENU_BTN_CIRCLE, "" },
        { 3000, OP_CAPTURE, 0, "first-2-pcs" },
        { 3200, OP_PRESS, MENU_BTN_R1, "" }, /* pcs -> games (error state) */
        { 3800, OP_CAPTURE, 0, "first-3-games-error" },
        { 4000, OP_PRESS, MENU_BTN_CIRCLE, "" }, /* back to PCs */
        { 4600, OP_CAPTURE, 0, "first-4-pcs" },
        /* leave via Settings -> Exit */
        { 4800, OP_PRESS, MENU_BTN_R1, "" }, /* pcs -> games */
        { 5000, OP_PRESS, MENU_BTN_R1, "" }, /* games -> settings */
        { 5200, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5230, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5260, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5290, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5320, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5350, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5380, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5410, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5440, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5470, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5500, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5530, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5560, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5590, OP_PRESS, MENU_BTN_DOWN, "" },
        { 5650, OP_PRESS, MENU_BTN_CROSS, "" },
        { 5900, OP_SLEEP, 0, "" },
    };
    app_config_t cfg;
    base_cfg(&cfg, 5);
    cfg.host[0] = '\0';
    cfg.wol_mac[0] = '\0';
    gs_server_t server;
    make_server(&server, "127.0.0.1", 0, 0);
    harness_gs_refresh(HARNESS_GS_FAIL_FAST);
    int rc = run_menu_with_steps(&cfg, NULL, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    (void)server;
    check("first boot: menu exits cleanly", rc, UI_MENU_EXIT);
}

/* Games loading -> error state with an empty grid (slow refresh keeps
 * the working state on screen for the first capture, then failures pile
 * up and auto-retry), then recovery when the refresh succeeds. */
static void scenario_games_loading(void) {
    static step_t steps[] = {
        { 400, OP_CAPTURE, 0, "load-0-loading" },
        { 1600, OP_GS, HARNESS_GS_FAIL_FAST, "" },
        { 3500, OP_CAPTURE, 0, "load-1-error" },
        { 4200, OP_GS, HARNESS_GS_OK, "" },
        { 5600, OP_CAPTURE, 0, "load-2-recovered" },
        /* leave via Settings -> Exit */
        { 5800, OP_PRESS, MENU_BTN_R1, "" },
        { 6000, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6030, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6060, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6090, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6120, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6150, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6180, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6210, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6240, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6270, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6300, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6330, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6360, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6390, OP_PRESS, MENU_BTN_DOWN, "" },
        { 6420, OP_PRESS, MENU_BTN_CROSS, "" },
        { 6700, OP_SLEEP, 0, "" },
    };
    app_config_t cfg;
    base_cfg(&cfg, 5);
    remove(FIXDIR "/applist.cache");
    gs_server_t server;
    make_server(&server, "127.0.0.1", 1, 1003);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, &server, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    check("loading/error exit", rc, UI_MENU_EXIT);
}

/* Pad absence notice, then recovery. */
static void scenario_no_pad(void) {
    static step_t steps[] = {
        { 1300, OP_CAPTURE, 0, "nopad-0" },
        { 1500, OP_NOPAD, 0, "" },
        { 2200, OP_CAPTURE, 0, "nopad-1-recovered" },
        /* leave via Settings -> Exit */
        { 2400, OP_PRESS, MENU_BTN_R1, "" },
        { 2600, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2630, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2660, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2690, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2720, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2750, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2780, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2810, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2840, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2870, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2900, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2930, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2960, OP_PRESS, MENU_BTN_DOWN, "" },
        { 2990, OP_PRESS, MENU_BTN_DOWN, "" },
        { 3050, OP_PRESS, MENU_BTN_CROSS, "" },
        { 3200, OP_SLEEP, 0, "" },
    };
    harness_set_no_pad(1);
    app_config_t cfg;
    base_cfg(&cfg, 5);
    gs_server_t server;
    make_server(&server, "127.0.0.1", 1, 1003);
    write_cache(FIXDIR, 0);
    harness_gs_refresh(HARNESS_GS_FAIL_SLOW);
    int rc = run_menu_with_steps(&cfg, &server, FIXDIR, steps,
                                 (int)(sizeof(steps) / sizeof(steps[0])));
    harness_set_no_pad(0);
    check("no-pad recovery exit", rc, UI_MENU_EXIT);
}

/* ------------------------------------------------------------------ */
/* launch / status screens (direct calls)                                */
/* ------------------------------------------------------------------ */

static void *launch_worker(void *arg) {
    launch_progress_t *p = arg;
    static const char *phases[] = {
        "Asking your PC to start the game...",
        "Reserving the stream...",
        "Shaking hands with the PC...",
    };
    for (int i = 0; i < 8; i++) {
        if (p->cancelled) {
            snprintf((char *)p->phase, sizeof(p->phase), "Cancelling...");
            usleep(800 * 1000);
            p->rc = -2;
            p->done = 1;
            return NULL;
        }
        snprintf((char *)p->phase, sizeof(p->phase), "%s",
                 phases[i % 3]);
        usleep(700 * 1000);
    }
    p->rc = 0;
    p->done = 1;
    return NULL;
}

typedef struct {
    const char *game;
    const char *verb;
    launch_progress_t *prog;
} launch_args_t;

static void launch_screen_call(void *p) {
    launch_args_t *a = p;
    ui_launch_screen_run(a->game, a->verb, a->prog);
}

static void scenario_launch_screen(void) {
    launch_progress_t prog;
    memset(&prog, 0, sizeof(prog));
    static step_t steps[] = {
        { 600, OP_CAPTURE, 0, "launch-0" },
        { 1500, OP_CAPTURE, 0, "launch-1" },
        { 2300, OP_PRESS, MENU_BTN_CIRCLE, "" }, /* cancel */
        { 2900, OP_CAPTURE, 0, "launch-2-cancel" },
        { 3600, OP_SLEEP, 0, "" },
    };
    launch_args_t args = { "Cinderfall Tactics", NULL, &prog };
    pthread_t w;
    pthread_create(&w, NULL, launch_worker, &prog);
    run_blocking_with_steps(launch_screen_call, &args, steps, 5);
    pthread_join(w, NULL);
    check("launch screen: worker cancelled rc", prog.rc, -2);
}

typedef struct {
    const char *game;
    const char *reason;
    int allow_refresh;
    int rc;
} fail_args_t;

static void fail_screen_call(void *p) {
    fail_args_t *a = p;
    a->rc = ui_launch_fail_screen_run(a->game, a->reason, a->allow_refresh);
}

static void scenario_fail_screen(void) {
    static step_t steps[] = {
        { 800, OP_CAPTURE, 0, "fail-0" },
        { 1600, OP_PRESS, MENU_BTN_CROSS, "" },
        { 2200, OP_SLEEP, 0, "" },
    };
    fail_args_t a = {
        "Kestrel Run",
        "Connection refused: the PC is reachable but Sunshine is not listening.",
        1,
        0,
    };
    run_blocking_with_steps(fail_screen_call, &a, steps, 3);
    check("fail screen X => retry", a.rc, UI_LAUNCH_FAIL_RETRY);
}

static void scenario_splash_and_status(void) {
    for (int t = 0; t < 6; t++) {
        ui_show_splash(t);
        char name[32];
        snprintf(name, sizeof(name), "splash-%d", t);
        harness_capture(name);
    }
    ui_show_status("PAIR", "PIN: 4821",
                   "Type this PIN on your PC in Sunshine's pairing page.");
    harness_capture("status-pair");
    ui_show_status("ERROR", "Could not connect",
                   "The PC did not answer. Check that Sunshine is running.");
    harness_capture("status-error");
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char *shotdir = argc > 1 ? argv[1] : "build-tests/shots";
    mkdir(shotdir, 0755);
    mkdir(FIXDIR, 0755);
    harness_set_shot_dir(shotdir);

    printf("== scenario: splash + status ==\n");
    scenario_splash_and_status();

    printf("== scenario: launch screen ==\n");
    scenario_launch_screen();

    printf("== scenario: launch fail screen ==\n");
    scenario_fail_screen();

    for (int t = 0; t < 6; t++) {
        printf("== scenario: theme tour %d (%s) ==\n", t, ui_theme_name(t));
        scenario_theme_tour(t);
    }

    printf("== scenario: osk flow ==\n");
    scenario_osk();

    printf("== scenario: games launch ==\n");
    scenario_games_launch();

    printf("== scenario: long grid ==\n");
    scenario_games_long();

    printf("== scenario: pcs offline/wol ==\n");
    scenario_pcs_offline();

    printf("== scenario: first boot ==\n");
    scenario_first_boot();

    printf("== scenario: loading/error/recover ==\n");
    scenario_games_loading();

    printf("== scenario: no pad ==\n");
    scenario_no_pad();

    if (g_failures) {
        printf("== %d assertion(s) failed ==\n", g_failures);
        return 1;
    }
    printf("== all scenarios passed ==\n");
    return 0;
}

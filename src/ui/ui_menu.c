#include "ui_menu.h"
#include "ui_draw.h"
#include "ui_theme.h"
#include "ui_art.h"
#include "../log.h"
#include "../input/input_pad.h"
#include "../video/video.h"
#include "../stream.h"
#include "../gamestream/gs_errors.h"
#include "../gamestream/discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#ifdef __ORBIS__
#include <orbis/SystemService.h>
#endif

#define UI_W 1920
#define UI_H 1080
#define UI_MAX_APPS 256

/* Colors 0xAARRGGBB (BGRA LE). */
#define COL_BG      (ui_theme_current()->bg)
#define COL_PANEL   (ui_theme_current()->panel)
#define COL_SEL     (ui_theme_current()->selected)
#define COL_ACCENT  (ui_theme_current()->accent)
#define COL_TEXT    (ui_theme_current()->text)
#define COL_DIM     (ui_theme_current()->dim)
#define COL_WARN    (ui_theme_current()->warn)

static unsigned long long menu_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ull + tv.tv_usec / 1000ull;
}

static int s_splash_hidden;
static char s_notice[160];
void ui_menu_set_notice(const char *message) {
    snprintf(s_notice, sizeof(s_notice), "%s", message ? message : "");
}

/* Display name of the game being launched (set on tile click, consumed by
 * stream.c's launch screen). Numeric app ids make for a poor title. */
static char s_pending_game[128];
void ui_menu_set_pending_game(const char *name) {
    snprintf(s_pending_game, sizeof(s_pending_game), "%s",
             name && name[0] ? name : "");
}
const char *ui_menu_take_pending_game(void) {
    return s_pending_game;
}

static void ui_hide_splash_once(void) {
#ifdef __ORBIS__
    if (s_splash_hidden)
        return;
    (void)sceSystemServiceHideSplashScreen();
    s_splash_hidden = 1;
#endif
}

void ui_show_splash(int theme) {
    ui_theme_set(theme);
    if (video_ui_begin(UI_W, UI_H) != 0) return;
    uint8_t *frame = malloc((size_t)UI_W * UI_H * 4);
    if (!frame) return;
    ui_surface_t surface = {frame, UI_W * 4, UI_W, UI_H};
    ui_theme_splash(&surface);
    if (video_ui_present(frame, UI_W * 4) == 0) ui_hide_splash_once();
    free(frame);
}

void ui_show_status(const char *title, const char *line1, const char *line2) {
    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("ui_show_status: video_ui_begin failed");
        ui_hide_splash_once(); /* better than eternal splash */
        return;
    }

    size_t pitch = (size_t)UI_W * 4u;
    size_t bytes = pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(bytes);
    if (!staging) {
        ui_hide_splash_once();
        return;
    }

    ui_surface_t s = { staging, (int)pitch, UI_W, UI_H };
    ui_theme_background(&s);
    ui_text(&s, 80, 80, 5, COL_TEXT, title ? title : "MOONLIGHT");
    if (line1 && line1[0])
        ui_text(&s, 80, 220, 4, COL_ACCENT, line1);
    if (line2 && line2[0])
        ui_text(&s, 80, 320, 3, COL_DIM, line2);
    ui_text(&s, 80, UI_H - 60, 2, COL_DIM, "Please wait...");

    for (int i = 0; i < 4; i++) {
        (void)video_ui_present(staging, (int)pitch);
        usleep(16000);
    }
    ui_hide_splash_once();
    free(staging);
    LOGI("ui_show_status: '%s' | '%s' | '%s'",
         title ? title : "", line1 ? line1 : "", line2 ? line2 : "");
}

/* --- animated launch screen ----------------------------------
 *
 * Clicking a game used to leave the LAST MENU FRAME frozen on screen for
 * the whole launch ladder + RTSP/ENet handshake (tens of seconds of fully
 * bounded attempts with zero feedback - the classic "the app
 * froze when I clicked a game" symptom). While the ladder runs on a worker thread
 * (see stream.c:run_launch_threaded), this screen shows the game name,
 * the worker's live phase text, a PS4-style orbiting-dots spinner and an
 * elapsed-seconds counter at ~30 fps, and lets CIRCLE cancel: the worker
 * checks prog->cancelled between its bounded steps.
 */
static uint32_t launch_mix(uint32_t under, uint32_t over, int alpha) {
    unsigned inv = 255 - (unsigned)alpha;
    return 0xff000000u |
        (((((under >> 16) & 255) * inv + ((over >> 16) & 255) * (unsigned)alpha) / 255) << 16) |
        (((((under >> 8) & 255) * inv + ((over >> 8) & 255) * (unsigned)alpha) / 255) << 8) |
        (((under & 255) * inv + (over & 255) * (unsigned)alpha) / 255);
}

void ui_launch_screen_run(const char *game, const char *verb,
                           launch_progress_t *prog) {
    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("launch screen: video_ui_begin failed");
        return;
    }
    size_t pitch = (size_t)UI_W * 4u;
    size_t bytes = pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(bytes);
    if (!staging) {
        LOGE("launch screen: staging malloc failed");
        return;
    }

    const char *name = (game && game[0]) ? game : "your game";
    const char *head = (verb && verb[0]) ? verb : "STARTING";
    unsigned long long start = menu_now_ms();
    int frame = 0;
    int cancel_requested = 0;

    LOGI("launch screen: '%s' (%s)", name, head);
    while (!prog->done) {
        unsigned pressed = 0, held = 0;
        if (input_menu_poll(&pressed, &held) == 0 && (pressed & MENU_BTN_CIRCLE)) {
            if (!cancel_requested) {
                cancel_requested = 1;
                prog->cancelled = 1;
                LOGI("launch screen: cancel requested by user");
            }
        }

        ui_surface_t s = { staging, (int)pitch, UI_W, UI_H };
        ui_theme_background(&s);

        ui_text(&s, 80, 250, 3, COL_DIM, head);
        ui_label_fit(&s, 80, 310, 60, 1, COL_TEXT, name, 1760);
        if (prog->phase[0])
            ui_label_fit(&s, 80, 450, 30, 0, COL_ACCENT, prog->phase, 1760);

        /* PS4-style orbiting spinner: eight dots at fixed 45-degree spots,
         * the fading trail rotates through them (no libm, no cos/sin). */
        {
            static const int spin_dx[8] = {1024, 724, 0, -724, -1024, -724, 0, 724};
            static const int spin_dy[8] = {0, 724, 1024, 724, 0, -724, -1024, -724};
            int cx = UI_W / 2, cy = 690, radius = 40;
            int slot = (frame / 2) % 8; /* full turn every 16 frames ~0.5 s */
            for (int k = 0; k < 8; k++) {
                int pos = (slot + k) & 7;
                int alpha = 255 - k * 28;
                ui_circle(&s, cx + radius * spin_dx[pos] / 1024,
                          cy + radius * spin_dy[pos] / 1024, 7,
                          launch_mix(COL_BG, COL_ACCENT, alpha));
            }
        }

        unsigned long long elapsed_s = (menu_now_ms() - start) / 1000u;
        char line[96];
        if (cancel_requested)
            snprintf(line, sizeof(line), "Cancelling...  %llu s", elapsed_s);
        else
            snprintf(line, sizeof(line), "%llu s", elapsed_s);
        ui_text(&s, 80, UI_H - 130, 2, COL_DIM, line);

        /* Hint bar: CIRCLE cancels. */
        ui_icon(&s, UI_W - 300, UI_H - 118, 38, 6, cancel_requested ? COL_DIM : 0xffff727d);
        ui_label(&s, UI_W - 244, UI_H - 122, 26, 0, cancel_requested ? COL_DIM : COL_TEXT,
                 "Cancel");

        (void)video_ui_present(staging, (int)pitch);
        frame++;
        usleep(33000); /* ~30 fps */
    }

    free(staging);
    input_menu_absorb();
    LOGI("launch screen: done rc=%d after %llu ms", prog->rc,
         menu_now_ms() - start);
}

int ui_launch_fail_screen_run(const char *game, const char *reason,
                              int allow_refresh) {
    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("launch fail screen: video_ui_begin failed");
        return UI_LAUNCH_FAIL_BACK;
    }
    size_t pitch = (size_t)UI_W * 4u;
    size_t bytes = pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(bytes);
    if (!staging) {
        LOGE("launch fail screen: staging malloc failed");
        return UI_LAUNCH_FAIL_BACK;
    }
    const char *name = (game && game[0]) ? game : "your game";
    LOGI("launch fail screen: '%s'%s", name, allow_refresh ? "" : " (no refresh)");

    input_menu_absorb();
    for (;;) {
        unsigned pressed = 0, held = 0;
        if (input_menu_poll(&pressed, &held) == 0) {
            if (pressed & MENU_BTN_CROSS) {
                free(staging);
                input_menu_absorb();
                return UI_LAUNCH_FAIL_RETRY;
            }
            if (allow_refresh && (pressed & MENU_BTN_TRIANGLE)) {
                free(staging);
                input_menu_absorb();
                return UI_LAUNCH_FAIL_REFRESH;
            }
            if (pressed & MENU_BTN_CIRCLE) {
                free(staging);
                input_menu_absorb();
                return UI_LAUNCH_FAIL_BACK;
            }
        }

        ui_surface_t s = { staging, (int)pitch, UI_W, UI_H };
        ui_theme_background(&s);

        ui_text(&s, 80, 250, 3, COL_WARN, "COULD NOT START");
        ui_label_fit(&s, 80, 310, 60, 1, COL_TEXT, name, 1760);
        if (reason && reason[0])
            ui_label_fit(&s, 80, 450, 30, 0, COL_ACCENT, reason, 1760);

        ui_label_fit(&s, 80, 560, 30, 1, COL_TEXT,
                     allow_refresh
                         ? "Could not start the stream. Press X to try again, TRIANGLE to refresh."
                         : "Could not start the stream. Press X to try again.",
                     1760);

        /* Keycap hints, mirroring the launch screen's cancel hint. */
        int hx = 80, hy = UI_H - 122;
        ui_icon(&s, hx, hy, 38, 7, 0xff65bcff);
        ui_label(&s, hx + 52, hy + 4, 26, 0, COL_TEXT, "Retry");
        hx += 250;
        ui_icon(&s, hx, hy, 38, 8, allow_refresh ? 0xff69e4bd : COL_DIM);
        ui_label(&s, hx + 52, hy + 4, 26, 0, allow_refresh ? COL_TEXT : COL_DIM,
                 "Refresh");
        if (allow_refresh) {
            hx += 250;
            ui_icon(&s, hx, hy, 38, 6, 0xffff727d);
            ui_label(&s, hx + 52, hy + 4, 26, 0, COL_TEXT, "Back");
        }

        (void)video_ui_present(staging, (int)pitch);
        usleep(33000); /* ~30 fps */
    }
}

typedef enum {
    TAB_PCS = 0,
    TAB_APPS = 1,
    TAB_SETTINGS = 2,
} ui_tab_t;

enum {
    SET_HOST = 0,
    SET_DEBUG_HOST,
    SET_WOL_MAC,
    SET_RES,
    SET_FPS,
    SET_BITRATE,
    SET_SOPS,
    SET_LOCAL_AUDIO,
    SET_PREFER_HW,
    SET_PREFER_YCBCR,
    SET_FILE_LOG,
    SET_SHOW_STATS,
    SET_SURROUND,
    SET_TOUCHPAD_MOUSE,
    SET_GYRO,
    SET_AUTOSTART,
    SET_DECODE_MODE,
    SET_THEME,
    SET_EXIT,
    SET_COUNT,
};

static const char *k_set_names[SET_COUNT] = {
    "PC IP address",
    "Debug host",
    "Wake-on-LAN MAC",
    "Resolution",
    "FPS",
    "Bitrate (kbps)",
    "SOPS",
    "Local audio",
    "Hardware decoding",
    "YCbCr (experimental)",
    "File logging",
    "Performance overlay",
    "Surround sound (5.1)",
    "Touchpad mouse",
    "Gyro motion input",
    "Auto-start game",
    "Decode mode",
    "Theme",
    "Exit Moonlight",
};

/* On-screen keyboards: IPv4 needs digits + dot; the Wake-on-LAN MAC
 * needs hex digits (separators optional). */
#define OSK_COLS 3
static const char k_osk_ip[4][OSK_COLS + 1] = {
    "123", "456", "789", ".0."
};
static const char k_osk_hex[6][OSK_COLS + 1] = {
    "123", "456", "789", "ABC", "DEF", ".0:"
};

/* Only settings needed for personal play are exposed. The Exit row gives
 * the app a sanctioned death: Settings -> Exit runs the PS4-native exit
 * (sceSystemServiceLoadExec "exit"), which never trips the crash dialog,
 * unlike closing from the quick menu (an uncatchable kill - see main.c). */
static const int k_visible_settings[] = {
    SET_HOST, SET_WOL_MAC, SET_AUTOSTART, SET_THEME, SET_RES, SET_FPS,
    SET_BITRATE, SET_SURROUND, SET_PREFER_HW, SET_SHOW_STATS,
    SET_TOUCHPAD_MOUSE, SET_GYRO, SET_FILE_LOG, SET_DECODE_MODE, SET_EXIT
};
#define VISIBLE_SETTINGS 15


typedef struct {
    char name[CONFIG_MAX_APP];
    int id;
} ui_app_t;

typedef struct {
    app_config_t *cfg;
    gs_server_t *server;
    const char *config_dir;

    ui_app_t apps[UI_MAX_APPS];
    int napps;
    mdns_host_t hosts[MDNS_MAX_HOSTS];
    int nhosts, sel_host;
    char notice[160];
    int apps_err; /* 0 ok, 1 applist failed, 2 no connection */
    int refresh_pending; /* background refresh running */
    int refresh_retries;    /* auto-retry budget (failures with no list) */
    int refresh_retry_wait; /* menu frames until the auto-retry fires */

    ui_tab_t tab;
    int sel_app;
    int app_first; /* scroll */
    int sel_set;
    /* the settings list scrolls - 96px rows (up from 52px/26px text;
     * 14 rows no longer fit at that size). set_scroll eases toward
     * set_scroll_target every menu frame until they match. */
    int set_scroll;
    int set_scroll_target;

    int osk_active;
    int osk_target; /* SET_HOST / SET_DEBUG_HOST / SET_WOL_MAC */
    int osk_row, osk_col;
    int osk_rows;  /* 4 (IP) or 6 (hex MAC) */
    char osk_buf[CONFIG_MAX_HOST];

    int host_dirty; /* host changed: reconnect required */
    int result;     /* -1000 = stay in loop */

    unsigned long long focus_anim_start; /* selection changed at this ms */

    struct apps_job *job; /* background applist refresh */

    /* network scan runs on a worker thread; the PCS tab shows
     * live results (the old scan blocked the menu for as
     * long as the kernel SYN budget on Orbis — measured on
     * hardware — with the pad dead on a "Scanning..." screen). */
    scan_job_t *scan;
    char scan_phase[48];
} ui_state_t;

/* --- application list: persistent cache + background refresh ---
 *
 * The list is cached in <config_dir>/applist.cache so the menu can render
 * instantly and only hit the network in a worker thread. It used to block
 * the first interactive frame on a full serverinfo + applist round trip
 * (two fresh TLS connections) on every launch.
 */

#define APPS_CACHE_FILE "applist.cache"

typedef struct apps_job {
    pthread_mutex_t mtx;
    int done;      /* results are ready */
    int abandoned; /* menu exited; the thread frees the job */
    int ok;        /* 0 = refresh succeeded */
    int apps_err;  /* 1 applist failed, 2 no connection */
    int napps;
    ui_app_t apps[UI_MAX_APPS];
    int status_ok;   /* serverinfo succeeded */
    int current_game;
    int paired;
    char host_mac[24]; /* PC MAC from the paired HTTPS serverinfo */
    /* Captured at spawn time (main thread): the worker never touches
     * gs_server_t, which the main thread may re-initialize later. */
    client_identity_t *id;
    char host[CONFIG_MAX_HOST];
    unsigned short http_port;
    unsigned short https_port;
} apps_job_t;

static void apps_cache_path(const ui_state_t *st, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", st->config_dir, APPS_CACHE_FILE);
}

static void apps_sanitize_name(char *name) {
    for (char *p = name; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
}

static int apps_load_cache(ui_state_t *st) {
    st->napps = 0;
    if (!st->cfg->host[0])
        return -1;
    char path[512], line[CONFIG_MAX_APP + 32];
    apps_cache_path(st, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    /* Header: "host <host>" — a cache from another host is ignored.
     * fgets() keeps the trailing '\n', so strip it before comparing:
     * an earlier revision compared raw ("192.168.2.99\n") and never matched,
     * which silently disabled the whole disk cache (the list then
     * depended entirely on the network refresh landing). */
    if (!fgets(line, sizeof(line), f) || strncmp(line, "host ", 5) != 0) {
        fclose(f);
        LOGW("ui: applist cache unreadable (bad header)");
        return -1;
    }
    {
        char *nl = strpbrk(line + 5, "\r\n");
        if (nl)
            *nl = '\0';
        if (strcmp(st->cfg->host, line + 5) != 0) {
            fclose(f);
            LOGW("ui: applist cache is for host '%s' (cfg '%s')",
                 line + 5, st->cfg->host);
            return -1;
        }
    }
    while (st->napps < UI_MAX_APPS && fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        int id = atoi(line);
        if (id <= 0)
            continue;
        snprintf(st->apps[st->napps].name, sizeof(st->apps[st->napps].name),
                 "%s", tab + 1);
        st->apps[st->napps].id = id;
        st->napps++;
    }
    fclose(f);
    LOGI("ui: applist cache %d apps", st->napps);
    return st->napps ? 0 : -1;
}

static void apps_save_cache(const ui_state_t *st) {
    if (!st->napps || !st->cfg->host[0])
        return;
    char path[512], tmp[540];
    apps_cache_path(st, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "host %s\n", st->cfg->host);
    for (int i = 0; i < st->napps; i++) {
        char name[CONFIG_MAX_APP];
        snprintf(name, sizeof(name), "%s", st->apps[i].name);
        apps_sanitize_name(name);
        fprintf(f, "%d\t%s\n", st->apps[i].id, name);
    }
    fclose(f);
    if (rename(tmp, path) != 0)
        remove(tmp);
}

static void apps_job_free(apps_job_t *j) {
    pthread_mutex_destroy(&j->mtx);
    free(j);
}

/* Defined with the settings helpers below; used by the refresh poller so
 * the menu-side WOL MAC adoption can save immediately. */
static void save_cfg(ui_state_t *st);

static void *apps_job_thread(void *arg) {
    /* games first — and there is nothing else: the
     * optional Windows companion (:47991 /open + Steam catalog sync)
     * is removed entirely. This job is the whole
     * refresh: serverinfo status + applist, both Sunshine endpoints. */
    apps_job_t *j = arg;
    app_entry_t *list = NULL;

    /* Best-effort status refresh, mirroring the old fetch_apps(). */
    bool paired = false;
    int current_game = 0;
    j->status_ok = gs_serverinfo_status(j->id, j->host, j->https_port,
                                        j->http_port, &paired,
                                        &current_game,
                                        j->host_mac, sizeof(j->host_mac)) == GS_OK;
    j->paired = paired;
    j->current_game = current_game;

    int rc = gs_applist_ex(j->id, j->host, j->https_port, &list);
    int n = 0;
    if (rc == GS_OK) {
        for (app_entry_t *a = list; a && n < UI_MAX_APPS; a = a->next) {
            snprintf(j->apps[n].name, sizeof(j->apps[n].name), "%s",
                     a->name ? a->name : "?");
            char *trademark = strstr(j->apps[n].name, "\xE2\x84\xA2");
            if (trademark)
                memmove(trademark, trademark + 3, strlen(trademark + 3) + 1);
            j->apps[n].id = a->id;
            n++;
        }
        xml_applist_free(list);
    }
    j->napps = n;
    j->ok = rc == GS_OK;
    j->apps_err = rc == GS_OK ? 0 : 1;

    pthread_mutex_lock(&j->mtx);
    j->done = 1;
    int free_it = j->abandoned;
    pthread_mutex_unlock(&j->mtx);
    if (free_it)
        apps_job_free(j); /* menu left before consuming the results */
    return NULL;
}

/* Consume finished refresh results. Returns 1 if the UI must redraw. */
static int apps_job_poll(ui_state_t *st) {
    apps_job_t *j = st->job;
    if (!j)
        return 0;
    pthread_mutex_lock(&j->mtx);
    if (!j->done) {
        pthread_mutex_unlock(&j->mtx);
        return 0;
    }
    st->refresh_pending = 0;
    if (j->ok) {
        st->napps = j->napps;
        memcpy(st->apps, j->apps, sizeof(ui_app_t) * (size_t)j->napps);
        st->apps_err = 0;
        st->refresh_retry_wait = 0; /* list landed; stop any retry timer */
        apps_save_cache(st);
        if (!strcmp(st->notice, "Refreshing games..."))
            st->notice[0] = 0;
        LOGI("ui: applist refreshed %d apps currentGame=%d", st->napps,
             j->current_game);
    } else if (st->napps == 0) {
        /* No cached content to fall back on. */
        st->apps_err = 1;
        snprintf(st->notice, sizeof(st->notice),
                 "Could not refresh games: %s", gs_error ? gs_error : "network error");
        /* Auto-retry: a transient blip at boot (companion probe holding
         * the HTTP mutex, host briefly busy) should not leave the grid
         * empty until the user discovers TRIANGLE. Two retries, ~1.5 s
         * apart at the menu cadence. */
        if (st->refresh_retries < 2) {
            st->refresh_retries++;
            st->refresh_retry_wait = 200;
            LOGI("ui: games refresh failed; auto-retry %d scheduled",
                 st->refresh_retries);
        }
    } else {
        /* Keep the cached list; surface the failure without blocking. */
        snprintf(st->notice, sizeof(st->notice),
                 "Could not refresh games: %s", gs_error ? gs_error : "network error");
        LOGW("ui: applist refresh failed: %s", gs_error ? gs_error : "?");
    }
    if (j->status_ok && st->server) {
        st->server->currentGame = j->current_game;
        st->server->paired = j->paired != 0;
        /* keep the live server's MAC current and adopt it for
         * Wake-on-LAN right in the menu (syncing only at connect
         * time left a user who paired but had not streamed yet - or
         * whose value sat un-flagged - without the "(auto)" tag). */
        if (j->host_mac[0]) {
            snprintf(st->server->host_mac, sizeof(st->server->host_mac),
                     "%s", j->host_mac);
            if (strcmp(j->host_mac, st->cfg->wol_mac) != 0 ||
                !st->cfg->wol_auto) {
                snprintf(st->cfg->wol_mac, sizeof(st->cfg->wol_mac),
                         "%s", j->host_mac);
                st->cfg->wol_auto = true;
                save_cfg(st);
                LOGI("ui: Wake-on-LAN MAC auto-detected: %s (saved)",
                     st->cfg->wol_mac);
            }
        }
    }
    pthread_mutex_unlock(&j->mtx);
    apps_job_free(j);
    st->job = NULL;
    return 1;
}

/* Launch the background refresh (no-op if one is already running). */
static void fetch_apps_async(ui_state_t *st) {
    if (st->job)
        return;
    if (!st->server) {
        st->apps_err = 2;
        return;
    }
    apps_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return; /* keep whatever the cache gave us */
    pthread_mutex_init(&j->mtx, NULL);
    j->id = st->server->id;
    snprintf(j->host, sizeof(j->host), "%s", st->server->address);
    j->http_port = st->server->httpPort;
    j->https_port = st->server->httpsPort;
    st->job = j;
    st->refresh_pending = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, apps_job_thread, j) == 0) {
        pthread_detach(t);
    } else {
        /* Thread creation failed (never seen on PS4): finish inline. */
        st->job = NULL;
        apps_job_free(j);
        st->refresh_pending = 0;
        (void)gs_refresh_status(st->server);
        app_entry_t *list = NULL;
        if (gs_applist(st->server, &list) == GS_OK) {
            int n = 0;
            for (app_entry_t *a = list; a && n < UI_MAX_APPS; a = a->next) {
                snprintf(st->apps[n].name, sizeof(st->apps[n].name), "%s",
                         a->name ? a->name : "?");
                st->apps[n].id = a->id;
                n++;
            }
            st->napps = n;
            st->apps_err = 0;
            apps_save_cache(st);
        } else {
            st->apps_err = 1;
        }
        xml_applist_free(list);
    }
}

static const char *active_app_name(const ui_state_t *st) {
    if (!st->server || st->server->currentGame == 0)
        return NULL;
    for (int i = 0; i < st->napps; i++) {
        if (st->apps[i].id == st->server->currentGame)
            return st->apps[i].name;
    }
    return NULL;
}

static void quit_active_stream(ui_state_t *st) {
    if (!st->server || st->server->currentGame == 0)
        return;
    int id = st->server->currentGame;
    const char *name = active_app_name(st);
    LOGI("ui: closing active stream id=%d name='%s'", id, name ? name : "?");
    if (gs_quit_app(st->server) != GS_OK) {
        LOGW("ui: gs_quit_app failed: %s", gs_error ? gs_error : "?");
        (void)gs_refresh_status(st->server);
    } else {
        LOGI("ui: stream closed");
        LOGN("Stream closed");
    }
}

static void preselect_app(ui_state_t *st) {
    st->sel_app = 0;
    for (int i = 0; i < st->napps; i++) {
        if (!strcasecmp(st->apps[i].name, st->cfg->app_name) ||
            atoi(st->cfg->app_name) == st->apps[i].id) {
            st->sel_app = i;
            break;
        }
    }
}

/* --- setting values as text --- */

static void set_value_str(const ui_state_t *st, int row, char *out, size_t cap) {
    const app_config_t *c = st->cfg;
    switch (row) {
    case SET_THEME: snprintf(out, cap, "%s", ui_theme_name(c->ui_theme)); break;
    case SET_DECODE_MODE: snprintf(out, cap, "%s", c->dec_pipeline_depth > 1 ? "Balanced" : "Low latency"); break;
    case SET_AUTOSTART: {
        if (!c->autostart || !c->app_name[0]) {
            snprintf(out, cap, "off");
        } else {
            /* Show the game's display name when the list has it. */
            const char *name = NULL;
            for (int i = 0; i < st->napps; i++) {
                if (atoi(c->app_name) == st->apps[i].id) {
                    name = st->apps[i].name;
                    break;
                }
            }
            snprintf(out, cap, "%s", name ? name : c->app_name);
        }
        break;
    }
    case SET_HOST:        snprintf(out, cap, "%s", c->host); break;
    case SET_DEBUG_HOST:  snprintf(out, cap, "%s", c->debug_host); break;
    case SET_WOL_MAC: {
        /* "(auto)" comes from the PERSISTED provenance flag
         * (stream.c / the menu refresh set it whenever the PC's own
         * serverinfo value was adopted), so the label is right even
         * after a fresh boot or when this menu session has no live
         * serverinfo at hand. A manual OSK entry clears the flag. */
        if (!c->wol_mac[0]) {
            snprintf(out, cap, "off");
        } else if (c->wol_auto) {
            snprintf(out, cap, "%s (auto)", c->wol_mac);
        } else {
            snprintf(out, cap, "%s", c->wol_mac);
        }
        break;
    }
    case SET_RES:         snprintf(out, cap, "%dx%d", c->stream.width, c->stream.height); break;
    case SET_FPS:         snprintf(out, cap, "%d", c->stream.fps); break;
    case SET_BITRATE:     snprintf(out, cap, "%d", c->stream.bitrate); break;
    case SET_SOPS:        snprintf(out, cap, "%s", c->sops ? "yes" : "no"); break;
    case SET_LOCAL_AUDIO: snprintf(out, cap, "%s", c->local_audio ? "yes" : "no"); break;
    case SET_PREFER_HW:   snprintf(out, cap, "%s", c->prefer_hw ? "yes" : "no"); break;
    case SET_PREFER_YCBCR:snprintf(out, cap, "%s", c->prefer_ycbcr ? "yes" : "no"); break;
    case SET_FILE_LOG:    snprintf(out, cap, "%s", c->enable_file_log ? "yes" : "no"); break;
    case SET_SHOW_STATS:  snprintf(out, cap, "%s", c->show_stats ? "yes" : "no"); break;
    case SET_SURROUND:    snprintf(out, cap, "%s", c->surround51 ? "yes (5.1)" : "no (stereo)"); break;
    case SET_TOUCHPAD_MOUSE: snprintf(out, cap, "%s", c->touchpad_mouse ? "yes" : "no"); break;
    case SET_GYRO:        snprintf(out, cap, "%s", c->gyro_motion ? "yes" : "no"); break;
    case SET_EXIT:        snprintf(out, cap, "Press X to exit"); break;
    default: out[0] = '\0'; break;
    }
}

static void cycle_res(app_config_t *c, int dir) {
    static const int presets[][2] = { {1280, 720}, {1920, 1080} };
    const int n = 2;
    int cur = 1;
    for (int i = 0; i < n; i++) {
        if (c->stream.width == presets[i][0] && c->stream.height == presets[i][1]) {
            cur = i;
            break;
        }
    }
    cur = (cur + dir + n) % n;
    c->stream.width = presets[cur][0];
    c->stream.height = presets[cur][1];
}

static void save_cfg(ui_state_t *st) {
    if (config_save(st->cfg, st->config_dir) != 0)
        LOGW("ui: config_save failed");
}

/* --- OSK helpers --- */

static char osk_char_at(const ui_state_t *st, int row, int col) {
    if (st->osk_rows > 4)
        return k_osk_hex[row][col];
    return k_osk_ip[row][col];
}

static void osk_open(ui_state_t *st, int target) {
    st->osk_active = 1;
    st->osk_target = target;
    st->osk_row = 0;
    st->osk_col = 0;
    st->osk_rows = (target == SET_WOL_MAC) ? 6 : 4;
    const char *cur = NULL;
    if (target == SET_HOST)
        cur = st->cfg->host;
    else if (target == SET_DEBUG_HOST)
        cur = st->cfg->debug_host;
    else if (target == SET_WOL_MAC)
        cur = st->cfg->wol_mac;
    /* no MAC saved yet but the PC already told us one (paired
     * serverinfo)? Pre-fill it - the user can still edit or clear. */
    if (target == SET_WOL_MAC && !cur && st->server && st->server->host_mac[0])
        cur = st->server->host_mac;
    snprintf(st->osk_buf, sizeof(st->osk_buf), "%s", cur ? cur : "");
}

static void osk_accept(ui_state_t *st) {
    if (st->osk_target == SET_WOL_MAC) {
        uint8_t mac[6];
        if (!st->osk_buf[0]) {
            st->cfg->wol_mac[0] = '\0';
            st->cfg->wol_auto = false; /* manual clear = manual */
            snprintf(st->notice, sizeof(st->notice),
                     "Wake-on-LAN cleared.");
        } else if (wol_mac_parse(st->osk_buf, mac) == 0) {
            wol_mac_format(mac, st->cfg->wol_mac, sizeof(st->cfg->wol_mac));
            /* typing by hand marks the value manual; if it matches
             * what the PC reports anyway, the next connect re-flags it
             * auto (stream.c) without changing a digit. */
            st->cfg->wol_auto = false;
            snprintf(st->notice, sizeof(st->notice),
                     "Wake-on-LAN MAC set to %s.", st->cfg->wol_mac);
        } else {
            snprintf(st->notice, sizeof(st->notice),
                     "Enter the PC's MAC, e.g. A1B2C3D4E5F6 (12 hex digits).");
            return; /* keep editing */
        }
        save_cfg(st);
        st->osk_active = 0;
        return;
    }
    if (!mdns_valid_ipv4(st->osk_buf)) {
        snprintf(st->notice, sizeof(st->notice), "Enter a valid PC IPv4 address (for example 192.168.1.100).");
        return;
    }
    st->notice[0] = 0;
    if (st->osk_target == SET_HOST) {
        if (strcmp(st->cfg->host, st->osk_buf) != 0) {
            snprintf(st->cfg->host, sizeof(st->cfg->host), "%s", st->osk_buf);
            st->cfg->http_port = 47989;
            st->host_dirty = 1;
        }
        save_cfg(st);
        st->osk_active = 0;
        /* After writing host: reconnect when leaving the keyboard. */
        if (st->cfg->host[0] && st->host_dirty)
            st->result = UI_MENU_RECONNECT;
        return;
    }
    snprintf(st->cfg->debug_host, sizeof(st->cfg->debug_host), "%s", st->osk_buf);
    save_cfg(st);
    st->osk_active = 0;
}

static void osk_input(ui_state_t *st, unsigned pr) {
    if (pr & MENU_BTN_UP)    st->osk_row = (st->osk_row + st->osk_rows - 1) % st->osk_rows;
    if (pr & MENU_BTN_DOWN)  st->osk_row = (st->osk_row + 1) % st->osk_rows;
    if (pr & MENU_BTN_LEFT)  st->osk_col = (st->osk_col + OSK_COLS - 1) % OSK_COLS;
    if (pr & MENU_BTN_RIGHT) st->osk_col = (st->osk_col + 1) % OSK_COLS;
    if (pr & MENU_BTN_CROSS) {
        size_t len = strlen(st->osk_buf);
        if (len + 1 < sizeof(st->osk_buf)) {
            st->osk_buf[len] = osk_char_at(st, st->osk_row, st->osk_col);
            st->osk_buf[len + 1] = '\0';
        }
    }
    if (pr & MENU_BTN_SQUARE) {
        size_t len = strlen(st->osk_buf);
        if (len)
            st->osk_buf[len - 1] = '\0';
    }
    if (pr & MENU_BTN_CIRCLE)
        st->osk_active = 0;
    if (pr & MENU_BTN_OPTIONS)
        osk_accept(st);
}

/* --- tab input --- */

/* Toggle the auto-start ("default") app. SQUARE on a game marks it as the
 * boot default; SQUARE again on the same game clears the default. */
static void toggle_default_app(ui_state_t *st) {
    if (st->napps <= 0)
        return;
    int id = st->apps[st->sel_app].id;
    const char *name = st->apps[st->sel_app].name;
    if (st->cfg->autostart && atoi(st->cfg->app_name) == id) {
        st->cfg->autostart = 0;
        snprintf(st->notice, sizeof(st->notice),
                 "Auto-start cleared. '%s' will no longer start at boot.", name);
        LOGI("ui: default app cleared (was %d)", id);
    } else {
        snprintf(st->cfg->app_name, sizeof(st->cfg->app_name), "%d", id);
        st->cfg->autostart = 1;
        snprintf(st->notice, sizeof(st->notice),
                 "'%s' is now the default: it starts automatically at boot.", name);
        LOGI("ui: default app => %d '%s'", id, name);
    }
    save_cfg(st);
}

static void launch_selected(ui_state_t *st) {
    if (st->napps > 0) {
        /* no /cancel of a different running app here anymore - the
         * launch flow's first step does it, visible on the animated launch
         * screen (this used to be a blocking HTTP call on the UI thread
         * with the last menu frame frozen on screen). */
        snprintf(st->cfg->app_name, sizeof(st->cfg->app_name), "%d",
                 st->apps[st->sel_app].id);
        ui_menu_set_pending_game(st->apps[st->sel_app].name);
    }
    save_cfg(st);
    LOGI("ui: launch app='%s' (sel=%d host_dirty=%d server=%d)",
         st->cfg->app_name, st->sel_app, st->host_dirty, st->server ? 1 : 0);
    /* No live server (e.g. cached list while disconnected): reconnect
     * first so the launch has a host to talk to.
     * - host changed: plain RECONNECT, the cached list may not even belong
     *   to the new host — land in the menu with a fresh list.
     * - same host, connection lost: RECONNECT_LAUNCH — the user's click
     *   means "play this", so reconnect and go straight into the stream
     *   instead of bouncing back into the menu for a second click. */
    st->result = st->host_dirty ? UI_MENU_RECONNECT :
                 (!st->server ? UI_MENU_RECONNECT_LAUNCH : UI_MENU_LAUNCH);
}

static void apps_input(ui_state_t *st, unsigned pr) {
    if ((pr & MENU_BTN_UP) && st->napps > 0) {
        st->sel_app = ui_theme_move_vertical(st->sel_app,st->napps,-1);
        st->focus_anim_start = menu_now_ms();
    }
    if ((pr & MENU_BTN_DOWN) && st->napps > 0) {
        st->sel_app = ui_theme_move_vertical(st->sel_app,st->napps,1);
        st->focus_anim_start = menu_now_ms();
    }
    if ((pr & MENU_BTN_LEFT) && st->napps > 0) {
        st->sel_app = (st->sel_app + st->napps - 1) % st->napps;
        st->focus_anim_start = menu_now_ms();
    }
    if ((pr & MENU_BTN_RIGHT) && st->napps > 0) {
        st->sel_app = (st->sel_app + 1) % st->napps;
        st->focus_anim_start = menu_now_ms();
    }

    if (pr & MENU_BTN_TRIANGLE) {
        if (st->host_dirty || !st->server) {
            save_cfg(st);
            st->result = UI_MENU_RECONNECT;
        } else if (st->job) {
            /* A refresh is already running (its requests are bounded but
             * can take seconds): say so instead of silently ignoring the
             * button, which read as "the app is broken". */
            snprintf(st->notice, sizeof(st->notice),
                     "Still refreshing games...");
        } else {
            snprintf(st->notice, sizeof(st->notice), "Refreshing games...");
            fetch_apps_async(st);
        }
    }
    /* X starts; O returns to the PC list. */
    if (pr & MENU_BTN_CIRCLE) st->tab = TAB_PCS;
    if (pr & MENU_BTN_SQUARE)
        toggle_default_app(st);
    if (pr & MENU_BTN_OPTIONS) {
        if (st->server && st->server->currentGame != 0)
            quit_active_stream(st);
    }
    if (pr & MENU_BTN_CROSS) {
        if (st->napps > 0)
            launch_selected(st);
        else if (st->apps_err)
            st->result = UI_MENU_RECONNECT;
    }
}

/* settings list geometry (shared by input + drawing).
 * Viewport: below the header, above the notice line. */
#define SET_ROW_H       96
#define SET_VIEW_Y0     216
#define SET_VIEW_H      728 /* 216..944, above the 948 notice line */
#define SET_VISIBLE_ROWS (SET_VIEW_H / SET_ROW_H) /* 7 full rows */

static void settings_clamp_scroll(ui_state_t *st) {
    int top = st->set_scroll_target;
    int sel_px = st->sel_set * SET_ROW_H;
    if (sel_px < top)
        top = sel_px; /* selection scrolled off the top: snap to it */
    else if (sel_px >= top + SET_VISIBLE_ROWS * SET_ROW_H)
        top = sel_px - (SET_VISIBLE_ROWS - 1) * SET_ROW_H; /* keep at bottom */
    int max_top = (VISIBLE_SETTINGS - SET_VISIBLE_ROWS) * SET_ROW_H;
    if (max_top < 0)
        max_top = 0;
    if (top > max_top)
        top = max_top;
    if (top < 0)
        top = 0;
    st->set_scroll_target = top;
}

static void settings_input(ui_state_t *st, unsigned pr) {
    app_config_t *c = st->cfg;
    int moved = 0;
    if (pr & MENU_BTN_UP) {
        st->sel_set = (st->sel_set + VISIBLE_SETTINGS - 1) % VISIBLE_SETTINGS;
        moved = 1;
    }
    if (pr & MENU_BTN_DOWN) {
        st->sel_set = (st->sel_set + 1) % VISIBLE_SETTINGS;
        moved = 1;
    }
    if (moved) {
        settings_clamp_scroll(st);
        st->focus_anim_start = menu_now_ms();
        return;
    }
    if (pr & MENU_BTN_CIRCLE) {
        st->tab = TAB_APPS;
        return;
    }

    int dir = 0;
    if (pr & MENU_BTN_LEFT) dir = -1;
    if (pr & MENU_BTN_RIGHT) dir = 1;
    int activate = (pr & MENU_BTN_CROSS) ? 1 : 0;
    if (!dir && !activate)
        return;

    int changed = 1;
    int setting = k_visible_settings[st->sel_set];
    switch (setting) {
    case SET_THEME:
        c->ui_theme = (c->ui_theme + (dir ? dir : 1) + UI_THEME_COUNT) % UI_THEME_COUNT;
        ui_theme_set(c->ui_theme);
        break;
    case SET_AUTOSTART:
        if (!c->app_name[0]) {
            snprintf(st->notice, sizeof(st->notice),
                     "Open Games and press SQUARE on a game to set the default first.");
            changed = 0;
            break;
        }
        c->autostart = !c->autostart;
        break;
    case SET_HOST:
    case SET_DEBUG_HOST:
    case SET_WOL_MAC:
        if (activate)
            osk_open(st, setting);
        changed = 0;
        break;
    case SET_RES:
        cycle_res(c, dir ? dir : 1);
        break;
    case SET_FPS:
        c->stream.fps = (c->stream.fps == 60) ? 30 : 60;
        break;
    case SET_BITRATE: {
        int b = c->stream.bitrate + (dir ? dir : 1) * 5000;
        if (b < 5000) b = 5000;
        if (b > 50000) b = 50000;
        c->stream.bitrate = b;
        break;
    }
    case SET_DECODE_MODE: c->dec_pipeline_depth = c->dec_pipeline_depth > 1 ? 1 : 2; break;
    case SET_SOPS:        c->sops = !c->sops; break;
    case SET_LOCAL_AUDIO: c->local_audio = !c->local_audio; break;
    case SET_PREFER_HW:   c->prefer_hw = !c->prefer_hw; break;
    case SET_PREFER_YCBCR:c->prefer_ycbcr = !c->prefer_ycbcr; break;
    case SET_FILE_LOG:
        c->enable_file_log = !c->enable_file_log;
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/debug.log", st->config_dir);
            log_set_file_enabled(c->enable_file_log ? 1 : 0, path);
        }
        break;
    case SET_SHOW_STATS:  c->show_stats = !c->show_stats; break;
    case SET_SURROUND:    c->surround51 = !c->surround51; break;
    case SET_TOUCHPAD_MOUSE: c->touchpad_mouse = !c->touchpad_mouse; break;
    case SET_GYRO:        c->gyro_motion = !c->gyro_motion; break;
    case SET_EXIT:
        if (activate)
            st->result = UI_MENU_EXIT;
        changed = 0;
        break;
    default: changed = 0; break;
    }
    if (changed)
        save_cfg(st);
}

/* --- drawing --- */

static void draw_osk(ui_state_t *st, ui_surface_t *s) {
    const int px = 480, py = 260, pw = 960, ph = 560;
    ui_rect(s, px - 4, py - 4, pw + 8, ph + 8, COL_ACCENT);
    ui_rect(s, px, py, pw, ph, COL_PANEL);

    const char *title = (st->osk_target == SET_WOL_MAC)
        ? "WAKE-ON-LAN MAC" : "PC IP ADDRESS";
    ui_text(s, px + 32, py + 28, 3, COL_ACCENT, title);

    /* Value with cursor */
    char line[CONFIG_MAX_HOST + 2];
    snprintf(line, sizeof(line), "%s_", st->osk_buf);
    ui_rect(s, px + 32, py + 88, pw - 64, 44, COL_BG);
    ui_text(s, px + 40, py + 98, 3, COL_TEXT, line);

    /* Grid: 6 rows for the hex MAC keyboard (smaller cells), 4 for IP. */
    const int rows = st->osk_rows;
    const int cell = rows > 4 ? 60 : 80;
    const int gx0 = px + pw / 2 - OSK_COLS * cell / 2;
    const int gy0 = py + (rows > 4 ? 150 : 170);
    for (int r = 0; r < rows; r++) {
        for (int cidx = 0; cidx < OSK_COLS; cidx++) {
            int x = gx0 + cidx * cell;
            int y = gy0 + r * cell;
            int sel = (r == st->osk_row && cidx == st->osk_col);
            ui_rect(s, x, y, cell - 8, cell - 8, sel ? COL_SEL : COL_BG);
            if (sel)
                ui_rect(s, x, y + cell - 12, cell - 8, 4, COL_ACCENT);
            char ch[2] = { osk_char_at(st, r, cidx), 0 };
            ui_text(s, x + (cell - 8) / 2 - 12, y + (cell - 8) / 2 - 12, 3,
                    sel ? COL_TEXT : COL_DIM, ch);
        }
    }

    ui_text(s, px + 32, py + ph - 40, 2, COL_DIM,
            rows > 4 ? "X add  [] backspace  O cancel  OPTIONS accept  (empty = off)"
                     : "X add   [] backspace   O cancel   OPTIONS accept");
}

#include "ui_design.inc"

/* one-line contextual help for the selected row, drawn under the
 * list where the host-changed warning lives. Makes the WOL auto-detect
 * state and the gyro toggle explain themselves without a manual. */
static const char *set_row_hint(const ui_state_t *st, int row) {
    switch (row) {
    case SET_WOL_MAC:
        if (st->cfg->wol_mac[0] && st->cfg->wol_auto)
            return "Auto-detected from your PC; a manual edit sticks until the PC reports a change.";
        if (st->cfg->wol_mac[0])
            return "Manual MAC - the PC's own value is re-adopted automatically after a paired connect.";
        if (st->server && st->server->paired)
            return "This PC is not reporting its MAC (old Sunshine, or VPN/bridge networking) - type it manually.";
        return "Sends a wake burst when a connect fails; auto-detected after one paired connect.";
    case SET_GYRO:
        return st->cfg->gyro_motion
            ? "Sends DualShock 4 gyro/accelerometer to the PC when a game asks (motion aiming)."
            : "Motion input off - sticks, buttons and touchpad are unaffected.";
    case SET_SHOW_STATS:
        return "In a stream, hold L3 + R3 2.5 s to open the quick menu "
               "(overlay lives there).";
    case SET_TOUCHPAD_MOUSE:
        return "One finger moves the host cursor; two-finger drag scrolls; two-finger click = right.";
    case SET_BITRATE:
        return "Higher = sharper image; 1080p/60 plays well around 20000.";
    default:
        return NULL;
    }
}

static void draw_settings(ui_state_t *st, ui_surface_t *s) {
    /* larger, scrollable list. The old layout crammed all 14 rows
     * on screen at 52px/26px ("the text is kinda small"); rows are 96px now
     * with 30px labels, 7 rows fit the viewport and the rest scroll - D-pad
     * auto-repeat walks the list and the view eases to follow. */
    int x = design_sidebar() ? 440 : 100;
    int list_w = UI_W - x - 64;

    for (int i = 0; i < VISIBLE_SETTINGS; i++) {
        int row_y = SET_VIEW_Y0 + i * SET_ROW_H - st->set_scroll;
        /* Only paint rows fully inside the viewport (no overdraw into the
         * notice line while the ease slides rows through the bottom). */
        if (row_y < SET_VIEW_Y0 - 1 || row_y + SET_ROW_H > SET_VIEW_Y0 + SET_VIEW_H + 1)
            continue;
        int sel = (i == st->sel_set);
        if (sel)
            ui_focus(s, x, row_y, list_w, SET_ROW_H - 12, 12, COL_SEL, 0);
        ui_label(s, x + 32, row_y + (SET_ROW_H - 12) / 2 - 16, 30, sel,
                 sel ? COL_TEXT : COL_DIM,
                 k_set_names[k_visible_settings[i]]);
        char val[192];
        set_value_str(st, k_visible_settings[i], val, sizeof(val));
        ui_label_fit(s, x + 700, row_y + (SET_ROW_H - 12) / 2 - 14, 28, 0,
                     sel ? COL_ACCENT : COL_DIM, val, list_w - 740);
    }

    /* Scrollbar (only when there is something to scroll). */
    int max_top = (VISIBLE_SETTINGS - SET_VISIBLE_ROWS) * SET_ROW_H;
    if (max_top > 0) {
        int track_x = UI_W - 44;
        int track_h = SET_VIEW_H - 24;
        ui_round_rect(s, track_x, SET_VIEW_Y0 + 12, 6, track_h, 3,
                      COL_PANEL);
        int thumb_h = track_h * SET_VISIBLE_ROWS / VISIBLE_SETTINGS;
        int thumb_y = SET_VIEW_Y0 + 12 +
                      (int64_t)track_h * st->set_scroll / max_top;
        if (thumb_y + thumb_h > SET_VIEW_Y0 + 12 + track_h)
            thumb_y = SET_VIEW_Y0 + 12 + track_h - thumb_h;
        ui_round_rect(s, track_x, thumb_y, 6, thumb_h, 3, COL_ACCENT);
    }

    if (st->host_dirty)
        ui_label_fit(s, x + 32, 952, 22, 0, COL_WARN,
                     "Host changed: open Games and press TRIANGLE to reconnect.",
                     list_w);
    else if (!st->notice[0]) {
        const char *hint = set_row_hint(st, k_visible_settings[st->sel_set]);
        if (hint)
            ui_label_fit(s, x + 32, 952, 22, 0, COL_DIM, hint, list_w);
    }
}

/* --- PCs tab: saved host first, probe refines it ----------------
 *
 * The list is seeded INSTANTLY from cfg->host — the PC the user last
 * connected to is always on the tab, zero network involved. The scan
 * worker then refines the entry: one plain-HTTP /serverinfo probe
 * (~250 ms to a live host) sets the online status and fills in the
 * PC's hostname. Subnet-wide discovery was removed after three
 * designs disproven on real hardware (see discovery.h); TRIANGLE re-checks the
 * saved PC. */
static void pcs_seed_saved(ui_state_t *st) {
    if (!st->cfg->host[0] || !mdns_valid_ipv4(st->cfg->host))
        return;
    if (st->nhosts >= MDNS_MAX_HOSTS)
        return;
    for (int i = 0; i < st->nhosts; i++)
        if (!strcmp(st->hosts[i].address, st->cfg->host))
            return; /* already listed */
    mdns_host_t *h = &st->hosts[st->nhosts++];
    memset(h, 0, sizeof(*h));
    snprintf(h->instance, sizeof(h->instance), "Gaming PC");
    snprintf(h->address, sizeof(h->address), "%s", st->cfg->host);
    h->port = st->cfg->http_port;
    h->online = -1; /* unknown until the probe lands */
}

static void scan_start(ui_state_t *st) {
    if (st->scan)
        return;
    st->scan = scan_job_begin(st->cfg->host[0] ? st->cfg->host : NULL);
    if (st->scan) {
        LOGI("ui: network scan started (hint='%s')",
             st->cfg->host[0] ? st->cfg->host : "-");
        snprintf(st->notice, sizeof(st->notice), "Checking for your PC...");
    }
}

/* Poll the scan worker: merge the probe's results into the seeded list
 * (update in place by address; append unknown addresses if any).
 * Returns 1 when the UI must redraw. */
static int scan_poll(ui_state_t *st) {
    if (!st->scan)
        return 0;
    mdns_host_t found[MDNS_MAX_HOSTS];
    found[0].address[0] = 0; /* scan_job_poll only writes n entries */
    int n = scan_job_poll(st->scan, found, MDNS_MAX_HOSTS,
                          st->scan_phase, sizeof(st->scan_phase));
    int redraw = 0;
    for (int i = 0; i < n; i++) {
        int hit = -1;
        for (int k = 0; k < st->nhosts; k++)
            if (!strcmp(st->hosts[k].address, found[i].address)) {
                hit = k;
                break;
            }
        if (hit < 0 && st->nhosts < MDNS_MAX_HOSTS) {
            hit = st->nhosts++;
            memset(&st->hosts[hit], 0, sizeof(st->hosts[hit]));
            snprintf(st->hosts[hit].address,
                     sizeof(st->hosts[hit].address), "%s", found[i].address);
            st->hosts[hit].port = found[i].port;
            redraw = 1;
        }
        if (hit >= 0 && found[i].instance[0] &&
            strcmp(st->hosts[hit].instance, found[i].instance) != 0) {
            snprintf(st->hosts[hit].instance,
                     sizeof(st->hosts[hit].instance), "%s", found[i].instance);
            redraw = 1;
        }
        if (hit >= 0 && st->hosts[hit].online != found[i].online) {
            st->hosts[hit].online = found[i].online;
            redraw = 1;
        }
    }
    if (scan_job_done(st->scan)) {
        scan_job_finish(&st->scan);
        LOGI("ui: network scan finished (%d hosts)", st->nhosts);
        redraw = 1;
    }
    return redraw;
}

static void pcs_input(ui_state_t *st, unsigned pr) {
    if (pr & MENU_BTN_TRIANGLE) {
        if (st->scan) {
            snprintf(st->notice, sizeof(st->notice), "Still checking... (%s)",
                     st->scan_phase[0] ? st->scan_phase : "working");
        } else {
            scan_start(st);
        }
    }
    /* SQUARE sends a Wake-on-LAN burst to the selected PC. Pure
     * UDP sendto() calls - cannot hit the console's TCP connect stall.
     * the saved MAC is normally auto-detected (paired HTTPS
     * serverinfo); the server-reported value is a fallback before the
     * first successful connect of this boot. */
    if ((pr & MENU_BTN_SQUARE) && st->nhosts) {
        mdns_host_t *h = &st->hosts[st->sel_host];
        const char *mac = st->cfg->wol_mac;
        if (!mac[0] && st->server && st->server->host_mac[0])
            mac = st->server->host_mac;
        if (!mac[0]) {
            snprintf(st->notice, sizeof(st->notice),
                     "To wake this PC, connect to it once - its MAC is then "
                     "detected automatically.");
        } else {
            int n = wol_send(mac, h->address);
            snprintf(st->notice, sizeof(st->notice),
                     n > 0
                         ? "Wake packet sent to %s. Give the PC ~15 s, then press TRIANGLE."
                         : "Could not send the wake packet.",
                     h->address);
        }
    }
    if (st->nhosts && (pr & MENU_BTN_UP))
        st->sel_host = (st->sel_host + st->nhosts - 1) % st->nhosts;
    if (st->nhosts && (pr & MENU_BTN_DOWN))
        st->sel_host = (st->sel_host + 1) % st->nhosts;
    if (pr & MENU_BTN_CROSS) {
        if (st->nhosts) {
            mdns_host_t *h = &st->hosts[st->sel_host];
            snprintf(st->cfg->host, sizeof(st->cfg->host), "%s", h->address);
            st->cfg->http_port = h->port;
            save_cfg(st);
            st->result = UI_MENU_RECONNECT;
        } else if (st->server) {
            /* Already connected: X just jumps to the games grid instead of
             * forcing a full reconnect round trip. */
            st->tab = TAB_APPS;
        } else if (st->cfg->host[0]) {
            st->result = UI_MENU_RECONNECT;
        } else { st->tab = TAB_SETTINGS; st->sel_set = 0; osk_open(st, SET_HOST); }
    }
}

static void draw_pcs(ui_state_t *st, ui_surface_t *s) {
    int x=design_sidebar()?440:100;
    ui_label(s, x+24, 250, 36, 1, COL_TEXT, "Your computers");
    ui_label(s, x+24, 310, 26, 0, COL_DIM, "The PC you connect to is remembered here.");
    if (st->scan) {
        /* Live check status (PS4-style quiet hint line). */
        char line[96];
        snprintf(line, sizeof(line), "Checking... %s",
                 st->scan_phase[0] ? st->scan_phase : "working");
        ui_label(s, x+24, 356, 22, 0, COL_ACCENT, line);
    } else if (!st->nhosts && !st->cfg->host[0]) {
        ui_label(s, x+24, 356, 22, 0, COL_DIM,
                 "Enter your PC's IP address to add it.");
    }
    if (!st->nhosts) {
        ui_round_rect(s, x, 380, UI_W-x-64, 200, 14, COL_PANEL);
        ui_label(s, x+36, 415, 32, 1, COL_TEXT, "Add your gaming PC");
        ui_label(s, x+36, 475, 26, 0, COL_DIM,
                 "X enter IP    TRIANGLE check again");
        return;
    }
    if (!st->scan && (st->cfg->wol_mac[0] ||
                      (st->server && st->server->host_mac[0]))) {
        ui_label(s, x+24, 356, 22, 0, COL_DIM,
                 "SQUARE sends a Wake-on-LAN packet to the selected PC");
    }
    int first = st->sel_host >= 7 ? st->sel_host - 6 : 0;
    for (int i = first; i < st->nhosts && i < first + 7; i++) {
        int y = 390 + (i-first) * 78;
        char name[64], line[80];
        const char *end = strstr(st->hosts[i].instance, "._nvstream");
        size_t len = end ? (size_t)(end - st->hosts[i].instance) : strlen(st->hosts[i].instance);
        if (len >= sizeof(name)) len = sizeof(name)-1;
        memcpy(name, st->hosts[i].instance, len); name[len] = 0;
        int sel = i == st->sel_host;
        ui_round_rect(s, x, y-12, UI_W-x-64, 70, 10, sel ? COL_SEL : COL_PANEL);
        ui_icon(s,x+24,y+4,36,0,sel ? COL_TEXT : COL_DIM);
        ui_label_fit(s, x+86, y, 30, 1, COL_TEXT, name, 640);
        /* Status: live session > probe verdict. */
        int connected = st->server && st->server->paired &&
                       !strcmp(st->server->address, st->hosts[i].address);
        int online = st->hosts[i].online;
        const char *status = connected ? "Connected"
                          : online == 1 ? "Online"
                          : online == 0 ? "Offline"
                          : "Checking...";
        uint32_t dot = connected || online == 1 ? 0xff42cc94
                     : online == 0 ? 0xffd9a441 : COL_DIM;
        ui_circle(s, x+780, y+17, 8, dot);
        ui_label(s, x+802, y+6, 24, 0,
                 connected ? COL_ACCENT : online == 1 ? COL_TEXT : COL_DIM,
                 status);
        snprintf(line, sizeof(line), "%s:%u", st->hosts[i].address, (unsigned)st->hosts[i].port);
        ui_label(s, UI_W-420, y+6, 24, 0, COL_DIM, line);
    }
}

static void draw_all(ui_state_t *st, ui_surface_t *s) {
    design_shell(st,s);
    if(st->tab==TAB_APPS)design_apps(st,s);
    else if(st->tab==TAB_PCS)draw_pcs(st,s);
    else draw_settings(st,s);
    design_footer(st,s);
    if(st->notice[0])ui_label_fit(s,design_sidebar()?440:64,948,20,0,COL_WARN,st->notice,1300);
    if(st->osk_active)draw_osk(st,s);
}
int ui_menu_run(app_config_t *cfg, gs_server_t *server, const char *config_dir) {
    ui_theme_set(cfg->ui_theme);
    ui_state_t st;
    memset(&st, 0, sizeof(st));
    st.cfg = cfg;
    st.server = server;
    st.config_dir = config_dir;
    st.result = -1000;
    snprintf(st.notice, sizeof(st.notice), "%s", s_notice);
    s_notice[0] = 0;
    /* No host / no server: open the PC list to configure. With a live
     * server, open straight on the games grid: the "PCs" detour (X to
     * reconnect, menu re-open, R1 to switch tabs) added seconds to every
     * launch for no benefit. */
    if (!cfg->host[0] || !server) {
        st.tab = TAB_PCS;
        st.sel_set = SET_HOST;
    } else {
        st.tab = TAB_APPS;
    }

    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("ui: video_ui_begin failed");
        return -1;
    }
    LOGI("ui: menu open (server=%s)", server ? "OK" : "NULL");

    ui_art_session_begin(server, config_dir);

    /* Cached list first, BEFORE the first paint: the grid is usable the
     * instant the menu appears; the network refresh runs in the
     * background and lands a moment later. Same for the PCs tab: the
     * saved host is listed instantly (no network), and the probe only
     * refines its status. */
    apps_load_cache(&st);
    pcs_seed_saved(&st);
    preselect_app(&st);

    /* Offscreen staging: drawing NEVER touches the on-screen buffer. */
    size_t stage_pitch = (size_t)UI_W * 4u;
    size_t stage_bytes = stage_pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(stage_bytes);
    if (!staging) {
        LOGE("ui: staging malloc failed");
        video_ui_end();
        return -1;
    }

    /* First frame NOW: don't hide splash until we have an image (avoids black). */
    {
        ui_surface_t s = { staging, (int)stage_pitch, UI_W, UI_H };
        draw_all(&st, &s);
        for (int i = 0; i < 3; i++) {
            if (video_ui_present(staging, (int)stage_pitch) == 0)
                break;
            usleep(8000);
        }
        ui_hide_splash_once();
    }

    /* Cached list shown above; kick off the network refresh now. */
    fetch_apps_async(&st);
    /* No live server: start the network scan right away (asynchronous -
     * a blocking scan would stall the menu thread for as long as the
     * kernel SYN budget on Orbis). With a saved host the scan uses it
     * as the hint, so the common case confirms the PC in milliseconds. */
    if (!server)
        scan_start(&st);

    /*
     * Back from a stream, buttons may still be held (e.g. X from the quick
     * menu's quit rows, or launch buttons after a drop). If s_menu_prev=0,
     * a held OPTIONS/X edge would relaunch instantly. Wait + absorb.
     */
    input_menu_wait_release(
        MENU_BTN_OPTIONS | MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_TRIANGLE,
        2000);
    input_menu_absorb();

    int dirty = 1;

    int present_fail = 0; /* consecutive present failures (app suspended?) */
    int grace = 45; /* ~0.7 s without accepting launch */

    while (st.result == -1000) {
        unsigned pr = 0, held = 0;
        if (input_menu_poll(&pr, &held) != 0) {
            snprintf(st.notice, sizeof(st.notice), "Connect a DualShock 4 to continue.");
            dirty = 1;
        } else if (!strcmp(st.notice, "Connect a DualShock 4 to continue.")) {
            st.notice[0] = 0; dirty = 1;
        }

        if (grace > 0) {
            grace--;
            /* During grace: navigate OK, but don't launch / change host. */
            pr &= ~(MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_OPTIONS |
                    MENU_BTN_TRIANGLE);
            if (held & (MENU_BTN_OPTIONS | MENU_BTN_CROSS | MENU_BTN_CIRCLE))
                input_menu_absorb();
        }

        if (pr) {
            if (st.osk_active)
                osk_input(&st, pr);
            else if (pr & (MENU_BTN_L1 | MENU_BTN_R1)) {
                st.tab = (ui_tab_t)((st.tab + ((pr & MENU_BTN_R1) ? 1 : 2)) % 3);
                /* entering Settings snaps the list to the selection
                 * (no visible easing from a stale scroll position). */
                if (st.tab == TAB_SETTINGS) {
                    settings_clamp_scroll(&st);
                    st.set_scroll = st.set_scroll_target;
                }
            }
            else if (st.tab == TAB_PCS)
                pcs_input(&st, pr);
            else if (st.tab == TAB_APPS)
                apps_input(&st, pr);
            else
                settings_input(&st, pr);
            dirty = 1;
        }

        /* Background work landing: apply refresh results / new art. */
        if (apps_job_poll(&st)) {
            if (st.sel_app >= st.napps)
                preselect_app(&st);
            dirty = 1;
        }
        if (ui_art_pump())
            dirty = 1;
        /* Live scan results: keep the phase line moving and the
         * found-PC list current while the worker runs. */
        if (scan_poll(&st))
            dirty = 1;

        /* Scheduled auto-retry for a failed refresh with an empty grid. */
        if (st.refresh_retry_wait > 0 && !st.job && !st.napps) {
            if (--st.refresh_retry_wait == 0) {
                LOGI("ui: auto-retrying games refresh");
                fetch_apps_async(&st);
                dirty = 1;
            }
        }

        /* Focus-settle animation: keep redrawing for a moment after a
         * selection change so the focused tile eases into place instead
         * of snapping (theme card layouts read st->focus_anim_start). */
        if (menu_now_ms() < st.focus_anim_start + 220)
            dirty = 1;

        /* settings list scroll easing - ~1/3 of the remaining
         * distance per frame (the earlier ~14% rate
         * read as sluggish): the view settles in ~5 frames (~100 ms at
         * the 8 ms menu cadence), still eased but decisive. Cap keeps
         * huge jumps from looking like a teleport. */
        if (st.tab == TAB_SETTINGS && !st.osk_active &&
            st.set_scroll != st.set_scroll_target) {
            int d = st.set_scroll_target - st.set_scroll;
            int step = d > 0 ? (d + 2) / 3 : -((-d + 2) / 3);
            if (step > 64) step = 64;
            if (step < -64) step = -64;
            if ((d > 0 && step > d) || (d < 0 && step < d))
                step = d;
            st.set_scroll += step;
            dirty = 1;
        }

        /* Exit NOW: don't redraw or flip (SubmitFlip VSYNC can hang
         * with high pending / cur=-1 â†’ eternal black screen). */
        if (st.result != -1000) {
            LOGI("ui: leaving menu (result=%d)", st.result);
            break;
        }

        /* Present on change only: the old loop redrew + flipped at
         * ~40 fps even with nothing moving, burning CPU on a console that
         * then had less headroom for the network threads. VideoOut retains
         * the last submitted frame, so a static menu needs no flips; every
         * state change (input, art, scan, refresh, animation window) sets
         * dirty. A display that keeps rejecting flips (quick menu opened /
         * suspending) is retried every loop until it accepts again. */
        if (dirty || present_fail) {
            ui_surface_t s = { staging, (int)stage_pitch, UI_W, UI_H };
            int skip_draw = present_fail >= 30;
            if (dirty && !skip_draw)
                draw_all(&st, &s);
            if (video_ui_present(staging, (int)stage_pitch) != 0) {
                present_fail++;
            } else {
                present_fail = 0;
                dirty = 0;
                ui_hide_splash_once();
            }
        }
        usleep(present_fail >= 30 ? 50000 : 8000);
    }

    LOGI("ui: freeing staging...");
    free(staging);

    /* Stop the art worker; decoded slots survive for the next menu. */
    ui_art_session_end();

    /* Detach or consume the in-flight refresh job. */
    if (st.job) {
        apps_job_t *j = st.job;
        pthread_mutex_lock(&j->mtx);
        int free_it = j->done;
        if (!free_it)
            j->abandoned = 1; /* the thread frees it after finishing */
        pthread_mutex_unlock(&j->mtx);
        if (free_it)
            apps_job_free(j);
        st.job = NULL;
    }

    /* Consume or abandon the network scan worker the same way. */
    if (st.scan)
        scan_job_finish(&st.scan);

    LOGI("ui: closing present...");
    video_ui_end();
    LOGI("ui: menu closed => %d (app='%s' host='%s')",
         st.result, cfg->app_name, cfg->host);
    return st.result;
}

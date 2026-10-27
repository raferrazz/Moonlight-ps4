/* Host replacements for the Orbis hardware edges used by ui_menu.c.
 * See host_harness.h for the contract. */
#include "host_harness.h"

#include "../../src/ui/ui_menu.h"
#include "../../src/input/input_pad.h"
#include "../../src/gamestream/client.h"
#include "../../src/gamestream/gs_errors.h"
#include "../../src/gamestream/gs_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Captured VideoOut                                                    */
/* ------------------------------------------------------------------ */

#define CAP_W 1920
#define CAP_H 1080

static pthread_mutex_t g_frame_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_frame; /* last presented frame (BGRA) */
static int g_active;
static int g_frames;
static long g_first_present_ms;
static char g_shot_dir[512];

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void harness_set_shot_dir(const char *dir) {
    snprintf(g_shot_dir, sizeof(g_shot_dir), "%s", dir);
}

static void ensure_frame(void) {
    if (!g_frame)
        g_frame = malloc((size_t)CAP_W * CAP_H * 4);
}

int video_ui_begin(int w, int h) {
    (void)w; (void)h;
    pthread_mutex_lock(&g_frame_mtx);
    ensure_frame();
    g_active = 1;
    pthread_mutex_unlock(&g_frame_mtx);
    return 0;
}

int video_ui_present(const uint8_t *src, int src_pitch) {
    pthread_mutex_lock(&g_frame_mtx);
    if (g_frame && g_active) {
        for (int y = 0; y < CAP_H; y++)
            memcpy(g_frame + (size_t)y * CAP_W * 4, src + (size_t)y * src_pitch,
                   CAP_W * 4);
        g_frames++;
        if (g_frames == 1)
            g_first_present_ms = now_ms();
    }
    pthread_mutex_unlock(&g_frame_mtx);
    return 0;
}

void video_ui_end(void) {
    pthread_mutex_lock(&g_frame_mtx);
    g_active = 0;
    pthread_mutex_unlock(&g_frame_mtx);
}

void harness_capture(const char *name) {
    pthread_mutex_lock(&g_frame_mtx);
    if (!g_frame || !g_shot_dir[0]) {
        pthread_mutex_unlock(&g_frame_mtx);
        return;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.ppm", g_shot_dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", CAP_W, CAP_H);
        for (int y = 0; y < CAP_H; y++) {
            const uint8_t *row = g_frame + (size_t)y * CAP_W * 4;
            for (int x = 0; x < CAP_W; x++) {
                uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_frame_mtx);
}

int harness_frames_presented(void) { return g_frames; }
void harness_reset_frame_count(void) {
    pthread_mutex_lock(&g_frame_mtx);
    g_frames = 0;
    g_first_present_ms = 0;
    pthread_mutex_unlock(&g_frame_mtx);
}
long harness_present_ms(void) {
    long first, n;
    pthread_mutex_lock(&g_frame_mtx);
    first = g_first_present_ms;
    n = g_frames;
    pthread_mutex_unlock(&g_frame_mtx);
    return n ? now_ms() - first : 0;
}

/* ------------------------------------------------------------------ */
/* Scripted pad                                                          */
/* ------------------------------------------------------------------ */

#define QUEUE_MAX 512
typedef struct {
    unsigned mask;
    long due_ms; /* monotonic ms; 0 = immediately */
} event_t;

static event_t g_queue[QUEUE_MAX];
static int g_q_count;
static int g_no_pad;
static pthread_mutex_t g_input_mtx = PTHREAD_MUTEX_INITIALIZER;

void harness_press(unsigned mask, int delay_ms) {
    pthread_mutex_lock(&g_input_mtx);
    if (g_q_count < QUEUE_MAX) {
        g_queue[g_q_count].mask = mask;
        g_queue[g_q_count].due_ms = delay_ms ? now_ms() + delay_ms : 0;
        g_q_count++;
    }
    pthread_mutex_unlock(&g_input_mtx);
}

void harness_clear_input(void) {
    pthread_mutex_lock(&g_input_mtx);
    g_q_count = 0;
    pthread_mutex_unlock(&g_input_mtx);
}

void harness_set_no_pad(int no_pad) { g_no_pad = no_pad; }
int harness_input_pending(void) {
    pthread_mutex_lock(&g_input_mtx);
    int n = g_q_count;
    pthread_mutex_unlock(&g_input_mtx);
    return n;
}

int input_menu_poll(unsigned *pressed, unsigned *held) {
    if (g_no_pad)
        return -1;
    if (pressed)
        *pressed = 0;
    if (held)
        *held = 0;
    pthread_mutex_lock(&g_input_mtx);
    for (int i = 0; i < g_q_count; i++) {
        if (g_queue[i].due_ms == 0 || g_queue[i].due_ms <= now_ms()) {
            if (pressed)
                *pressed = g_queue[i].mask;
            memmove(&g_queue[i], &g_queue[i + 1],
                    (size_t)(g_q_count - i - 1) * sizeof(event_t));
            g_q_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_input_mtx);
    return 0;
}

void input_menu_absorb(void) {}
void input_menu_wait_release(unsigned mask, int timeout_ms) {
    (void)mask; (void)timeout_ms;
}

/* ------------------------------------------------------------------ */
/* Deterministic gamestream client                                      */
/* ------------------------------------------------------------------ */

const char *gs_error = "connection refused (harness stub)";

int g_gs_mode = HARNESS_GS_FAIL_SLOW;
void harness_gs_refresh(int mode) { g_gs_mode = mode; }

static int stub_sleep_mode_ms(int slow_ms) {
    if (g_gs_mode == HARNESS_GS_FAIL_SLOW)
        usleep(slow_ms * 1000);
    else if (g_gs_mode == HARNESS_GS_FAIL_FAST)
        usleep(20 * 1000);
    return 0;
}

int gs_serverinfo_status(client_identity_t *id, const char *address,
                         unsigned short https_port, unsigned short http_port,
                         bool *paired, int *current_game,
                         char *host_mac_out, size_t host_mac_cap) {
    (void)id; (void)address; (void)https_port; (void)http_port;
    stub_sleep_mode_ms(600);
    if (g_gs_mode == HARNESS_GS_FAIL_FAST || g_gs_mode == HARNESS_GS_FAIL_SLOW)
        return GS_FAILED;
    if (paired)
        *paired = true;
    if (current_game)
        *current_game = 0;
    if (host_mac_out && host_mac_cap)
        snprintf(host_mac_out, host_mac_cap, "%s", "3C:97:0E:12:34:56");
    return GS_OK;
}

/* applist for HARNESS_GS_OK_LIST: 12 titles with ids 1000.. */
static const char *k_stub_apps[] = {
    "Asteroid Miners", "Blue Harbor", "Cinderfall Tactics", "Dust Circuit",
    "Ember Voyage", "Frostline Rally", "Gilded Depths", "Hollow Meridian",
    "Ivory Peaks", "Juniper Sky", "Kestrel Run", "Lumen Drift",
};

int gs_applist_ex(client_identity_t *id, const char *address,
                  unsigned short https_port, app_entry_t **list) {
    (void)id; (void)address; (void)https_port;
    stub_sleep_mode_ms(600);
    *list = NULL;
    if (g_gs_mode != HARNESS_GS_OK && g_gs_mode != HARNESS_GS_OK_LIST)
        return GS_FAILED;
    int n = g_gs_mode == HARNESS_GS_OK_LIST ? 12 : 0;
    app_entry_t *head = NULL, *tail = NULL;
    for (int i = 0; i < n; i++) {
        app_entry_t *e = calloc(1, sizeof(*e));
        if (!e)
            break;
        e->id = 1000 + i;
        e->name = strdup(k_stub_apps[i]);
        if (tail)
            tail->next = e;
        else
            head = e;
        tail = e;
    }
    *list = head;
    return GS_OK;
}

int gs_applist(gs_server_t *server, app_entry_t **list) {
    return gs_applist_ex(server->id, server->address, server->httpsPort, list);
}

void xml_applist_free(app_entry_t *list) {
    while (list) {
        app_entry_t *n = list->next;
        free((void *)list->name);
        free(list);
        list = n;
    }
}

int gs_quit_app(gs_server_t *server) {
    (void)server;
    return GS_OK;
}

int gs_refresh_status(gs_server_t *server) {
    (void)server;
    return GS_OK;
}

/* HTTP: the harness never fetches from the network; art misses land in the
 * placeholder path (which is exactly the state under test). */
int http_get(const char *host, unsigned short port, int tls, const char *path,
             http_resp_t *resp) {
    (void)host; (void)port; (void)tls; (void)path;
    memset(resp, 0, sizeof(*resp));
    return GS_FAILED;
}

void http_resp_free(http_resp_t *resp) {
    (void)resp;
}

/* moonlight-common-c default initializer (see Misc.c: a plain memset). */
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION streamConfig) {
    memset(streamConfig, 0, sizeof(*streamConfig));
}

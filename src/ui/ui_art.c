#include "ui_art.h"
#include "ui_theme.h"
#include "../log.h"
#include "../gamestream/gs_errors.h"
#include "../gamestream/client.h"
#include "../gamestream/gs_http.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../vendor/stb_image.h"
#pragma GCC diagnostic pop
#include "ui_lake_data.h"

/*
 * Cover-art store for the Games grid.
 *
 * The network fetch and image decode run on a worker thread
 * instead of the UI draw path. They used to run inside draw_all(): every
 * frame that touched an uncached card blocked on a full TCP+TLS round
 * trip plus a PNG decode, and nothing was persisted, so every launch
 * re-downloaded all art again.
 *
 * Completed art is persisted to <config_dir>/art/<appid> and fetched once
 * ever. The worker talks HTTP directly with host/port values captured at
 * menu open (safe point), so it never dereferences gs_server_t, which the
 * main thread may re-initialize while the menu is closed.
 *
 * UI thread: draw calls look up a READY slot and blit it; a miss marks the
 * slot WANTED and queues a load, drawing the landscape placeholder
 * meanwhile. ui_art_pump() reports arrivals so the menu can redraw. All
 * slot state transitions happen under one mutex; READY slot contents are
 * immutable until the UI thread evicts them.
 */

#define ART_SLOTS 12
#define ART_QUEUE 128
#define ART_HOST_MAX 128
#define ART_FAIL_BACKOFF_MS 2000

/* Square-tile render ladder (Games hero row): TILE_W(208)..HERO_W(448) in
 * 32 px steps, endpoints pinned - 9 distinct sizes. The row quantizes its
 * glide-time tile sizes onto this ladder so each (app, size) scaled
 * render happens ONCE and is then blitted (ui_blit_rgba) every frame;
 * the original approach resampled ~10 full-resolution tiles per glide
 * frame through the 64-bit premultiplied bilinear, which held the
 * console at ~24-32 fps. Worst-case memory: 12 slots x 9 sizes x ~330^2 x 4 B
 * ~= 46 MB, lazily filled and freed with the slot. */
#define ART_LADDER_MIN 208
#define ART_LADDER_MAX 448
#define ART_LADDER_STEP 32
#define ART_LADDER_N 9

int ui_art_ladder_snap(int size) {
    /* Floor to the 32 px grid (endpoint clamps at both ends). Floor, not
     * ceil: the packed-row geometry spaces tiles by their CONTINUOUS
     * widths, so a snapped-up tile could overhang its slot and touch its
     * neighbor mid-glide; snapped-down tiles only make the visual gap
     * breathe wider, which is invisible at glide speed. The settled row
     * stays pixel-exact: the endpoints ARE TILE_W (208) and HERO_W
     * (448), and every far tile sits at exactly TILE_W. */
    if (size <= ART_LADDER_MIN)
        return ART_LADDER_MIN;
    if (size >= ART_LADDER_MAX)
        return ART_LADDER_MAX;
    int k = (size - ART_LADDER_MIN) / ART_LADDER_STEP;
    return ART_LADDER_MIN + k * ART_LADDER_STEP;
}

static int lad_index(int snapped) {
    if (snapped == ART_LADDER_MAX)
        return ART_LADDER_N - 1;
    if (snapped < ART_LADDER_MIN || snapped > ART_LADDER_MAX)
        return -1;
    return (snapped - ART_LADDER_MIN) / ART_LADDER_STEP;
}

enum { ART_EMPTY = 0, ART_WANTED, ART_READY, ART_FAILED };

static struct {
    int id, state, w, h, hw, hh;
    unsigned stamp;
    unsigned long long fail_ms; /* FAILED backoff: no refetch storm */
    unsigned char *rgba, *hero;
    /* Ladder renders (ui_surface format, straight alpha, radius 10 as
     * drawn by the Games row). Immutable while READY; installed from the
     * worker (steady endpoints) or lazily from the draw path (sweep
     * sizes); freed with the slot. */
    uint32_t *lad[ART_LADDER_N];
} slots[ART_SLOTS];
static unsigned s_stamp;
static unsigned s_arrivals;

static unsigned long long art_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ull + tv.tv_usec / 1000ull;
}

/* Captured at menu open; the worker only uses these. */
static char s_host[ART_HOST_MAX];
static unsigned short s_https_port;
static char s_dir[512]; /* <config_dir>/art */

static pthread_t s_worker;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static int s_worker_alive; /* spawned once per process; sleeps while idle */

typedef struct {
    int app_id;
    char host[ART_HOST_MAX];
    unsigned short port;
} art_req_t;
static art_req_t s_queue[ART_QUEUE];
static int s_q_head, s_q_tail, s_q_count;

static unsigned char *landscape;
static int landscape_w, landscape_h;

static void art_mkdir(const char *path);

static void art_lad_free(int slot) {
    for (int k = 0; k < ART_LADDER_N; k++) {
        free(slots[slot].lad[k]);
        slots[slot].lad[k] = NULL;
    }
}

/* Render one cached ladder tile: scale the source art once into a
 * straight-alpha buffer (ui_image_render_rgba keeps corner coverage in the
 * alpha channel, so a later ui_blit_rgba composites exactly like a direct
 * draw would have - ui_image_round's output cannot be cached because it
 * bakes coverage into RGB against whatever was underneath). */
static uint32_t *art_render_scaled(const unsigned char *rgba, int iw, int ih,
                                   int size, int radius) {
    uint32_t *buf = malloc((size_t)size * (size_t)size * 4);
    if (!buf)
        return NULL;
    memset(buf, 0, (size_t)size * (size_t)size * 4);
    ui_image_render_rgba(buf, size, size, rgba, iw, ih, radius);
    return buf;
}

static uint32_t *s_land_lad[ART_LADDER_N]; /* landscape ladder renders */

void ui_landscape(ui_surface_t *surface,int x,int y,int w,int h,int radius) {
    if(!landscape) {
        int channels;
        landscape=stbi_load_from_memory(lake_png,sizeof(lake_png),&landscape_w,&landscape_h,&channels,4);
    }
    if(!landscape){ui_round_rect(surface,x,y,w,h,radius,ui_theme_current()->panel);return;}
    /* Ladder sizes (the Games row's Desktop tile and the placeholder
     * cover): blit the cached render instead of resampling the panorama
     * every glide frame. Filled with the caller's radius (10 from the
     * row); non-square callers (theme background) use the direct path. */
    if(w==h&&w>=ART_LADDER_MIN&&w<=ART_LADDER_MAX) {
        int li=lad_index(ui_art_ladder_snap(w));
        int size=li==ART_LADDER_N-1?ART_LADDER_MAX:ART_LADDER_MIN+li*ART_LADDER_STEP;
        if(li>=0) {
            if(!s_land_lad[li])
                s_land_lad[li]=art_render_scaled(landscape,landscape_w,landscape_h,size,10);
            if(s_land_lad[li]) {
                ui_blit_rgba(surface,x,y,size,size,s_land_lad[li],size);
                return;
            }
        }
    }
    ui_image_round(surface,x,y,w,h,landscape,landscape_w,landscape_h,radius);
}

static unsigned read_be32(const unsigned char *p) {
    return (unsigned)p[0]<<24 | (unsigned)p[1]<<16 | (unsigned)p[2]<<8 | p[3];
}
/* Optional embedded panorama in standard PNG; truncated/oversized chunks fail closed. */
static unsigned char *decode_hero(const unsigned char *png,size_t size,int *width,int *height) {
    if(size<8 || memcmp(png,"\x89PNG\r\n\x1a\n",8))return NULL;
    size_t pos=8;
    while(size-pos>=12) {
        unsigned len=read_be32(png+pos);
        if(len>size-pos-12)return NULL;
        if(!memcmp(png+pos+4,"mlBg",4)) {
            int w,h,c;
            if(len>INT_MAX||!stbi_info_from_memory(png+pos+8,(int)len,&w,&h,&c))return NULL;
            if(w<=0||h<=0||w>1920||h>1080||(size_t)w*h>2200000)return NULL;
            return stbi_load_from_memory(png+pos+8,(int)len,width,height,&c,4);
        }
        if(!memcmp(png+pos+4,"IEND",4))break;
        pos+=(size_t)len+12;
    }
    return NULL;
}

/* --- worker-side helpers (no locks held) --- */

static char *art_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static int art_write_file(const char *path, const unsigned char *buf, size_t len) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    if (n != len) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/* Fetch raw art bytes: disk cache first, then the host (and persist). */
static unsigned char *art_fetch(const art_req_t *req, size_t *len) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%d", s_dir, req->app_id);
    unsigned char *bytes = (unsigned char *)art_read_file(path, len);
    if (bytes) {
        LOGI("art: %d from cache (%zu bytes)", req->app_id, *len);
        return bytes;
    }
    if (!req->host[0] || !req->port)
        return NULL;
    char url[256];
    http_resp_t resp = {0};
    snprintf(url, sizeof(url),
             "/appasset?uniqueid=%s&appid=%d&AssetType=2&AssetIdx=0",
             GS_UNIQUEID, req->app_id);
    if (http_get(req->host, req->port, 1, url, &resp) != GS_OK) {
        http_resp_free(&resp);
        return NULL;
    }
    unsigned char *net = (unsigned char *)resp.body;
    size_t nlen = resp.len;
    if (!net || nlen == 0) {
        free(net);
        return NULL;
    }
    /* Only persist decodable images (PNG/JPEG magic) so a broken reply
     * does not poison the cache for future launches. */
    int looks_image = (nlen >= 8 &&
                       (!memcmp(net, "\x89PNG\r\n\x1a\n", 8) ||
                        (net[0] == 0xFF && net[1] == 0xD8)));
    if (looks_image && s_dir[0] && art_write_file(path, net, nlen) == 0)
        LOGI("art: %d fetched and cached (%zu bytes)", req->app_id, nlen);
    *len = nlen;
    return net;
}

static void art_slot_install(int slot, int app_id, unsigned char *rgba, int w, int h,
                             unsigned char *hero, int hw, int hh,
                             uint32_t *lad_small, uint32_t *lad_hero) {
    pthread_mutex_lock(&s_mtx);
    if (slots[slot].state == ART_WANTED && slots[slot].id == app_id) {
        slots[slot].rgba = rgba;
        slots[slot].w = w;
        slots[slot].h = h;
        slots[slot].hero = hero;
        slots[slot].hw = hw;
        slots[slot].hh = hh;
        slots[slot].state = ART_READY;
        slots[slot].stamp = ++s_stamp;
        slots[slot].lad[0] = lad_small;
        slots[slot].lad[ART_LADDER_N - 1] = lad_hero;
        s_arrivals++;
    } else {
        /* The UI evicted or re-targeted this slot meanwhile: drop the decode. */
        stbi_image_free(rgba);
        stbi_image_free(hero);
        free(lad_small);
        free(lad_hero);
    }
    pthread_mutex_unlock(&s_mtx);
}

static void art_load_slot(int slot, const art_req_t *req) {
    size_t len = 0;
    unsigned char *bytes = art_fetch(req, &len);
    if (!bytes || len > INT_MAX) {
        free(bytes);
        pthread_mutex_lock(&s_mtx);
        if (slots[slot].state == ART_WANTED && slots[slot].id == req->app_id) {
            slots[slot].state = ART_FAILED;
            slots[slot].fail_ms = art_now_ms();
        }
        pthread_mutex_unlock(&s_mtx);
        return;
    }
    unsigned char *rgba = NULL, *hero = NULL;
    int w = 0, h = 0, hw = 0, hh = 0, comp = 0;
    if (stbi_info_from_memory(bytes, (int)len, &w, &h, &comp) &&
        w > 0 && h > 0 && w <= 1200 && h <= 1800 && (size_t)w * h <= 2200000) {
        rgba = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4);
        hero = decode_hero(bytes, len, &hw, &hh);
    } else {
        LOGW("art: %d not a decodable image (%zu bytes)", req->app_id, len);
    }
    free(bytes);
    if (!rgba) {
        stbi_image_free(hero);
        pthread_mutex_lock(&s_mtx);
        if (slots[slot].state == ART_WANTED && slots[slot].id == req->app_id) {
            slots[slot].state = ART_FAILED;
            slots[slot].fail_ms = art_now_ms();
        }
        pthread_mutex_unlock(&s_mtx);
        return;
    }
    /* Prewarm the two steady ladder sizes (small tile + hero) on the
     * worker: the settled row never scales on the draw thread. The
     * in-between sweep sizes fill lazily on first use (once per app). */
    uint32_t *lad_small = art_render_scaled(rgba, w, h, ART_LADDER_MIN, 10);
    uint32_t *lad_hero = art_render_scaled(rgba, w, h, ART_LADDER_MAX, 10);
    art_slot_install(slot, req->app_id, rgba, w, h, hero, hw, hh,
                     lad_small, lad_hero);
}

static void *art_worker_main(void *arg) {
    (void)arg;
    for (;;) {
        art_req_t req;
        pthread_mutex_lock(&s_mtx);
        while (s_q_count == 0)
            pthread_cond_wait(&s_cv, &s_mtx);
        req = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % ART_QUEUE;
        s_q_count--;
        /* Resolve the slot while holding the lock; load without it. */
        int slot = -1;
        for (int i = 0; i < ART_SLOTS; i++)
            if (slots[i].state == ART_WANTED && slots[i].id == req.app_id)
                slot = i;
        pthread_mutex_unlock(&s_mtx);
        if (slot >= 0)
            art_load_slot(slot, &req);
    }
    return NULL;
}

static void art_queue_push(const art_req_t *req) {
    pthread_mutex_lock(&s_mtx);
    if (s_q_count < ART_QUEUE) {
        s_queue[s_q_tail] = *req;
        s_q_tail = (s_q_tail + 1) % ART_QUEUE;
        s_q_count++;
        pthread_cond_broadcast(&s_cv);
    }
    pthread_mutex_unlock(&s_mtx);
}

/* --- public API --- */

static void art_mkdir(const char *path) {
#ifdef __ORBIS__
    extern int sceKernelMkdir(const char *, int);
    sceKernelMkdir(path, 0777);
#else
    mkdir(path, 0755);
#endif
}

void ui_art_session_begin(gs_server_t *server, const char *config_dir) {
    /* Capture host/port at this safe point (main thread, menu stable). */
    pthread_mutex_lock(&s_mtx);
    if (server) {
        snprintf(s_host, sizeof(s_host), "%s", server->address);
        s_https_port = server->httpsPort;
    } else {
        s_host[0] = '\0';
        s_https_port = 0;
    }
    pthread_mutex_unlock(&s_mtx);

    if (config_dir && config_dir[0])
        snprintf(s_dir, sizeof(s_dir), "%s/art", config_dir);
    else
        s_dir[0] = '\0';
    if (s_dir[0])
        art_mkdir(s_dir);

    /* One worker for the whole process: it sleeps on the condvar between
     * menus, so there is no teardown/respawn race. (the worker is killed and
     * respawned it per menu open; a worker still finishing its last fetch
     * could outlive the flag reset and leak a second thread per cycle.) */
    if (!s_worker_alive) {
        s_q_head = s_q_tail = s_q_count = 0;
        if (pthread_create(&s_worker, NULL, art_worker_main, NULL) == 0) {
            s_worker_alive = 1;
            pthread_detach(s_worker);
        } else {
            LOGW("art: worker create failed; synchronous fallback");
        }
    }
}

void ui_art_session_end(void) {
    /* Just drain pending requests: the worker is process-lifetime and
     * sleeps until the next menu queues something. An in-flight fetch is
     * allowed to finish into the cache (harmless; READY slots persist
     * across menus by design). */
    pthread_mutex_lock(&s_mtx);
    s_q_head = s_q_tail = s_q_count = 0;
    pthread_mutex_unlock(&s_mtx);
}

int ui_art_pump(void) {
    int arrived = 0;
    pthread_mutex_lock(&s_mtx);
    if (s_arrivals) {
        arrived = 1;
        s_arrivals = 0;
    }
    pthread_mutex_unlock(&s_mtx);
    return arrived;
}

void ui_art_clear(void) {
    pthread_mutex_lock(&s_mtx);
    stbi_image_free(landscape);
    landscape = NULL;
    for (int k = 0; k < ART_LADDER_N; k++) {
        free(s_land_lad[k]);
        s_land_lad[k] = NULL;
    }
    for (int i = 0; i < ART_SLOTS; i++) {
        stbi_image_free(slots[i].rgba);
        stbi_image_free(slots[i].hero);
        slots[i].rgba = slots[i].hero = NULL;
        art_lad_free(i);
        slots[i].id = slots[i].w = slots[i].h = 0;
        slots[i].hw = slots[i].hh = 0;
        slots[i].state = ART_EMPTY;
        slots[i].stamp = 0;
    }
    s_stamp = 0;
    pthread_mutex_unlock(&s_mtx);
}

void ui_art_draw(gs_server_t *server, int app_id, ui_surface_t *surface,
                 int x, int y, int width, int height) {
    ui_art_draw_round(server,app_id,surface,x,y,width,height,0);
}

void ui_art_draw_round(gs_server_t *server, int app_id, ui_surface_t *surface,
                      int x, int y, int width, int height, int radius) {
    (void)server; /* host/port were captured in ui_art_session_begin */
    int ready = -1;
    int claimed = 0; /* this call transitioned a slot to WANTED */
    const unsigned char *rgba = NULL, *hero = NULL;
    int w = 0, h = 0, hw = 0, hh = 0;
    uint32_t *cached = NULL;
    int li = -1;
    /* Square ladder-size draws (the Games row) use the render cache;
    * every other shape scales directly as before. */
    if (width == height && width >= ART_LADDER_MIN &&
        width <= ART_LADDER_MAX) {
        int snapped = ui_art_ladder_snap(width);
        li = lad_index(snapped);
    }
    art_req_t req;

    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < ART_SLOTS; i++) {
        if (slots[i].id == app_id) {
            if (slots[i].state == ART_READY) {
                ready = i;
                /* Snapshot under the lock: contents are immutable while
                 * READY, and only this thread ever evicts them. */
                rgba = slots[i].rgba;
                w = slots[i].w;
                h = slots[i].h;
                hero = slots[i].hero;
                hw = slots[i].hw;
                hh = slots[i].hh;
                if (li >= 0)
                    cached = slots[i].lad[li];
                slots[i].stamp = ++s_stamp;
            }
            break;
        }
    }
    if (ready < 0) {
        /* A load is already in flight for this app (every draw
         * frame used to re-queue the same WANTED slot, tripling the
         * disk reads/decodes and flooding the log with "from cache"
         * lines), or the asset recently failed (backoff instead of a
         * per-frame refetch storm). Only a genuinely new id claims the
         * LRU slot and queues work. */
        int in_flight = 0, cooling = 0;
        for (int i = 0; i < ART_SLOTS; i++) {
            if (slots[i].id != app_id)
                continue;
            if (slots[i].state == ART_WANTED)
                in_flight = 1;
            else if (slots[i].state == ART_FAILED &&
                     art_now_ms() - slots[i].fail_ms < ART_FAIL_BACKOFF_MS)
                cooling = 1;
            else if (slots[i].state == ART_FAILED)
                slots[i].state = ART_EMPTY; /* backoff elapsed: retry */
        }
        if (!in_flight && !cooling) {
            int victim = 0;
            for (int i = 1; i < ART_SLOTS; i++)
                if (slots[i].state == ART_EMPTY ||
                    slots[i].stamp < slots[victim].stamp)
                    victim = i;
            stbi_image_free(slots[victim].rgba);
            stbi_image_free(slots[victim].hero);
            slots[victim].rgba = slots[victim].hero = NULL;
            art_lad_free(victim);
            slots[victim].id = app_id;
            slots[victim].w = slots[victim].h = 0;
            slots[victim].hw = slots[victim].hh = 0;
            slots[victim].state = ART_WANTED;
            slots[victim].stamp = ++s_stamp;
            slots[victim].fail_ms = 0;
            req.app_id = app_id;
            snprintf(req.host, sizeof(req.host), "%s", s_host);
            req.port = s_https_port;
            claimed = 1;
        }
    }
    pthread_mutex_unlock(&s_mtx);

    /* Ladder cache: blit the pre-rendered tile. On a miss, render this
     * (app, size) once off the lock and install it if the slot still
     * holds the app; the rare OOM falls through to the scaling path.
     * (The UI thread is the only evictor and the worker only installs
     * into WANTED slots, so a READY snapshot cannot go stale under
     * this call - the re-check is defensive.) */
    if (ready >= 0 && li >= 0 && rgba) {
        int size = li == ART_LADDER_N - 1 ? ART_LADDER_MAX
                                          : ART_LADDER_MIN + li * ART_LADDER_STEP;
        int single_use = 0;
        if (!cached) {
            cached = art_render_scaled(rgba, w, h, size, radius);
            if (cached) {
                int installed = 0;
                pthread_mutex_lock(&s_mtx);
                if (slots[ready].state == ART_READY &&
                    slots[ready].id == app_id && !slots[ready].lad[li]) {
                    slots[ready].lad[li] = cached;
                    installed = 1;
                }
                pthread_mutex_unlock(&s_mtx);
                if (!installed)
                    single_use = 1; /* another fill won: draw it, drop it */
            }
        }
        if (cached) {
            ui_blit_rgba(surface, x, y, size, size, cached, size);
            if (single_use)
                free(cached);
            return;
        }
    }

    if (rgba || hero) {
        if (hero && width*10>height*11)
            ui_image_round(surface,x,y,width,height,hero,hw,hh,radius);
        else if (rgba)
            ui_image_round(surface,x,y,width,height,rgba,w,h,radius);
        return;
    }

    if (ready < 0 && claimed) {
        /* Newly queued: hand it to the worker (or load inline without one). */
        if (s_worker_alive) {
            art_queue_push(&req);
        } else {
            int slot = -1;
            pthread_mutex_lock(&s_mtx);
            for (int i = 0; i < ART_SLOTS; i++)
                if (slots[i].id == app_id && slots[i].state == ART_WANTED)
                    slot = i;
            pthread_mutex_unlock(&s_mtx);
            if (slot >= 0)
                art_load_slot(slot, &req);
        }
    }

    /* Placeholder cover: the shared lake panorama (a fixed dark night
     * scene on every theme) with the monitor pictogram dead-center. The
     * pictogram is always near-white: tinting it with the theme's text
     * color made it vanish on light themes (Daylight's dark ink blended
     * into the lake), and dark themes already draw white ink - one
     * constant keeps the glyph legible everywhere. */
    ui_landscape(surface,x,y,width,height,radius);
    ui_icon(surface,x+width/2-40,y+height/2-40,80,0,0xfff4f9ff);
}

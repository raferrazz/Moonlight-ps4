// Persistent client configuration (simple INI).
#pragma once

#include <stdbool.h>
#include <Limelight.h>

#define CONFIG_DIR_PS4 "/data/moonlight"
#define CONFIG_MAX_HOST 128
#define CONFIG_MAX_APP  128

/* Saved-PC list capacity (the PCs tab): up to 8 computers, most recently
 * used first. The [pcs] section of moonlight.ini is the store; the
 * legacy flat host/wol_mac keys remain as the ACTIVE PC mirror so every
 * pre-list code path (stream.c, the launch ladder, WoL) keeps working. */
#define CONFIG_MAX_PCS 8

typedef struct {
    char host[CONFIG_MAX_HOST]; /* IPv4 address (or hostname) */
    char name[64];              /* display name from /serverinfo; "" until probed */
    char mac[24];              /* auto-detected Wake-on-LAN MAC; "" = none */
    char mac_auto;             /* mac came from the host's serverinfo (vs. typed) */
} saved_pc_t;

typedef struct {
    STREAM_CONFIGURATION stream; // embedded: width/height/fps/bitrate/...

    char host[CONFIG_MAX_HOST];
    unsigned short http_port; // discovered SRV port; manual IP defaults to 47989
    char app_name[CONFIG_MAX_APP]; // name or numeric id
    char debug_host[64];

    bool sops;
    bool local_audio;
    bool prefer_hw;
    bool videodec2_spike;
    bool prefer_ycbcr;
    bool enable_file_log; // writes /data/moonlight/debug.log
    bool show_stats; // on-screen perf overlay (FPS/decode/convert/present/KB per frame)
    bool paired_ok; // runtime only
    bool autostart; // boot straight into app_name (set with SQUARE on a game)
    int ui_theme; // Midnight, Mono, Blue Wave, Daylight, Cinema (0..4)

    /* Feature additions. */
    char wol_mac[24];      // "AA:BB:CC:DD:EE:FF"; empty = Wake-on-LAN off
                           // (mirror of the ACTIVE PC's mac below)
    bool wol_auto;         // wol_mac came from the host's serverinfo
                           // (shows "(auto)" in Settings; manual edit clears)
    int surround_mode;     // 0 stereo, 1 = 5.1, 2 = 7.1 (opt-in; falls back
                           // to a stereo downmix if 8ch output fails)
    bool touchpad_mouse;   // DS4 touchpad acts as a mouse (click=L, 2-finger=R)
    bool gyro_motion;      // forward DS4 gyro/accel to the host when a
                           // game asks (toggle; off = no motion caps announced)

    /* Saved PCs (PCs tab, MRU-first; see CONFIG_MAX_PCS). host/wol_mac
     * above always mirror pcs[pc_active] so single-host code is unaware. */
    saved_pc_t pcs[CONFIG_MAX_PCS];
    int pc_count;   // 0..CONFIG_MAX_PCS
    int pc_active;  // index of the PC cfg->host mirrors; -1 = none

    /* Videodec2 tuning, A/B-able on console without a rebuild. Rev-2 defaults
     * (depth=2 + AU ONION) are the fast path; files with cfg_rev < 2 get them
     * re-applied on load. See docs/CONSOLE_VALIDATE.md. */
    int dec_pipeline_depth; // frames in flight inside Videodec2 (1..VIDEODEC2_MAX_FB)
    int dec_thread_prio;    // Orbis priority of the Vdec CPU worker (lower = higher)
    int slices_per_frame;   // CAPABILITY_SLICES_PER_FRAME asked of the host
    bool dec_au_onion;      // AU bitstream in cacheable ONION (false = WC_GARLIC)
    bool dec_fb_garlic;     // force decoder framebuffer to WC_GARLIC (skip ONION alias)
    int bgra_workers;       // NV12->BGRA convert threads (1..6)
    int bgra_nt;            // -1 auto, 0 cached stores, 1 streaming stores
} app_config_t;

void config_set_defaults(app_config_t *cfg);
void config_ensure_dir(const char *dir);
int config_load(app_config_t *cfg, const char *dir);
int config_save(const app_config_t *cfg, const char *dir);

/* --- saved-PC list (moonlight.ini [pcs]) -------------------------------- */

/* Add or re-select a computer (dedup by host, case-insensitive). Moves it
 * to the MRU head, makes it active and mirrors host/wol_mac; when full,
 * the least-recently-used entry is dropped. Returns its index (0). */
int config_pc_add(app_config_t *cfg, const char *host, const char *name,
                  const char *mac);

/* Remove entry `index`; pc_active/host/wol_mac move to the new head (or
 * clear when the list empties). */
void config_pc_remove(app_config_t *cfg, int index);

/* Make entry `index` the active, most-recently-used PC (mirrors host and
 * wol_mac). */
void config_pc_touch(app_config_t *cfg, int index);

/* Persist a freshly learned MAC for the PC cfg->host mirrors (pairing,
 * serverinfo adoption). Mirrors wol_mac + wol_auto like the old path. */
void config_pc_note_mac(app_config_t *cfg, const char *mac);

/* Manual Wake-on-LAN entry (the Settings OSK): writes the active PC's
 * stored MAC with manual provenance (an empty string clears it). */
void config_pc_set_mac_manual(app_config_t *cfg, const char *mac);

/* Update the stored display name of the PC at `host` (the probed
 * hostname from the scan worker). */
void config_pc_note_name(app_config_t *cfg, const char *host, const char *name);

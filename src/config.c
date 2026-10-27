#include "config.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

void config_set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    LiInitializeStreamConfiguration(&cfg->stream);
    cfg->http_port = 47989;
    cfg->stream.width = 1920;
    cfg->stream.height = 1080;
    cfg->stream.fps = 60;
    cfg->stream.bitrate = 20000;
    /* Moonlight default. Smaller payloads mean more packets/s for the same
     * bitrate and a proportionally smaller SO_RCVBUF request upstream. */
    cfg->stream.packetSize = 1392;
    cfg->stream.streamingRemotely = STREAM_CFG_LOCAL;
    cfg->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
    cfg->stream.encryptionFlags = ENCFLG_NONE;
    cfg->stream.colorSpace = COLORSPACE_REC_709;
    cfg->stream.colorRange = COLOR_RANGE_LIMITED;
    cfg->stream.clientRefreshRateX100 = 5994;
    cfg->sops = true;
    cfg->local_audio = false;
    cfg->prefer_hw = true;
    cfg->videodec2_spike = false;
    cfg->prefer_ycbcr = false;
    cfg->enable_file_log = true; // default ON for fresh configs; saved configs keep their value
    cfg->show_stats = false;
    cfg->dec_pipeline_depth = 2;
    cfg->dec_thread_prio = 700;
    cfg->slices_per_frame = 2;
    cfg->dec_au_onion = true;
    cfg->dec_fb_garlic = false;
    cfg->bgra_workers = 4;
    cfg->bgra_nt = -1;
    cfg->autostart = false;
    cfg->ui_theme = 5; /* Orbis (PS4 system look) is the default */
    cfg->wol_mac[0] = '\0';      /* WOL off until the user sets a MAC */
    cfg->wol_auto = false;        /* provenance flag (host vs. manual) */
    cfg->surround_mode = 0;      /* stereo is the safe default on any TV/AVR */
    cfg->touchpad_mouse = true;  /* natural for Desktop; games never touch it */
    cfg->gyro_motion = true;     /* motion aiming works out of the box */
    cfg->pc_count = 0;
    cfg->pc_active = -1;
    snprintf(cfg->app_name, sizeof(cfg->app_name), "Steam Big Picture");
}

void config_ensure_dir(const char *dir) {
#ifdef __ORBIS__
    // Create basic parent dirs if missing (/data).
    sceKernelMkdir("/data", 0777);
    sceKernelMkdir(dir, 0777);
#else
    mkdir(dir, 0755);
#endif
    (void)errno;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int parse_bool(const char *v) {
    return !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes");
}

/* --- saved-PC list ------------------------------------------------ */

/* Mirror pcs[pc_active] into the flat host/wol_mac fields the rest of
 * the app reads. Clears them when there is no active PC. */
static void pc_mirror_active(app_config_t *cfg) {
    if (cfg->pc_active < 0 || cfg->pc_active >= cfg->pc_count) {
        cfg->pc_active = cfg->pc_count > 0 ? 0 : -1;
        if (cfg->pc_active < 0) {
            cfg->host[0] = '\0';
            cfg->wol_mac[0] = '\0';
            cfg->wol_auto = false;
            return;
        }
    }
    const saved_pc_t *pc = &cfg->pcs[cfg->pc_active];
    snprintf(cfg->host, sizeof(cfg->host), "%s", pc->host);
    snprintf(cfg->wol_mac, sizeof(cfg->wol_mac), "%s", pc->mac);
    cfg->wol_auto = pc->mac_auto != 0;
}

int config_pc_add(app_config_t *cfg, const char *host, const char *name,
                  const char *mac) {
    if (!host || !host[0])
        return -1;

    /* Dedup: an existing entry moves to the MRU head. */
    for (int i = 0; i < cfg->pc_count; i++) {
        if (!strcasecmp(cfg->pcs[i].host, host)) {
            saved_pc_t pc = cfg->pcs[i];
            if (name && name[0])
                snprintf(pc.name, sizeof(pc.name), "%s", name);
            if (mac && mac[0])
                snprintf(pc.mac, sizeof(pc.mac), "%s", mac);
            for (int k = i; k > 0; k--)
                cfg->pcs[k] = cfg->pcs[k - 1];
            cfg->pcs[0] = pc;
            cfg->pc_active = 0;
            pc_mirror_active(cfg);
            return 0;
        }
    }

    if (cfg->pc_count >= CONFIG_MAX_PCS) {
        /* Full: drop the least-recently-used tail. */
        cfg->pc_count = CONFIG_MAX_PCS - 1;
    }
    for (int k = cfg->pc_count; k > 0; k--)
        cfg->pcs[k] = cfg->pcs[k - 1];
    saved_pc_t *pc = &cfg->pcs[0];
    memset(pc, 0, sizeof(*pc));
    snprintf(pc->host, sizeof(pc->host), "%s", host);
    if (name && name[0])
        snprintf(pc->name, sizeof(pc->name), "%s", name);
    if (mac && mac[0])
        snprintf(pc->mac, sizeof(pc->mac), "%s", mac);
    cfg->pc_count++;
    cfg->pc_active = 0;
    pc_mirror_active(cfg);
    return 0;
}

void config_pc_remove(app_config_t *cfg, int index) {
    if (index < 0 || index >= cfg->pc_count)
        return;
    for (int k = index; k + 1 < cfg->pc_count; k++)
        cfg->pcs[k] = cfg->pcs[k + 1];
    cfg->pc_count--;
    memset(&cfg->pcs[cfg->pc_count], 0, sizeof(cfg->pcs[cfg->pc_count]));
    if (cfg->pc_active == index)
        cfg->pc_active = cfg->pc_count > 0 ? 0 : -1;
    else if (cfg->pc_active > index)
        cfg->pc_active--;
    pc_mirror_active(cfg);
}

void config_pc_touch(app_config_t *cfg, int index) {
    if (index < 0 || index >= cfg->pc_count)
        return;
    if (index == 0) {
        cfg->pc_active = 0;
        pc_mirror_active(cfg);
        return;
    }
    saved_pc_t pc = cfg->pcs[index];
    for (int k = index; k > 0; k--)
        cfg->pcs[k] = cfg->pcs[k - 1];
    cfg->pcs[0] = pc;
    cfg->pc_active = 0;
    pc_mirror_active(cfg);
}

void config_pc_note_mac(app_config_t *cfg, const char *mac) {
    if (!cfg->host[0])
        return;
    /* The entry matching the active host (case-insensitive) keeps the
     * per-PC MAC; the flat wol_mac mirror follows. */
    for (int i = 0; i < cfg->pc_count; i++) {
        if (!strcasecmp(cfg->pcs[i].host, cfg->host)) {
            snprintf(cfg->pcs[i].mac, sizeof(cfg->pcs[i].mac), "%s",
                     mac ? mac : "");
            cfg->pcs[i].mac_auto = (mac && mac[0]) ? 1 : 0;
            if (i == cfg->pc_active)
                break;
        }
    }
    snprintf(cfg->wol_mac, sizeof(cfg->wol_mac), "%s", mac ? mac : "");
    cfg->wol_auto = cfg->wol_mac[0] != '\0';
}

void config_pc_set_mac_manual(app_config_t *cfg, const char *mac) {
    if (!cfg->host[0])
        return;
    for (int i = 0; i < cfg->pc_count; i++) {
        if (!strcasecmp(cfg->pcs[i].host, cfg->host)) {
            snprintf(cfg->pcs[i].mac, sizeof(cfg->pcs[i].mac), "%s",
                     mac ? mac : "");
            cfg->pcs[i].mac_auto = 0;
            break;
        }
    }
    snprintf(cfg->wol_mac, sizeof(cfg->wol_mac), "%s", mac ? mac : "");
    cfg->wol_auto = false;
}

void config_pc_note_name(app_config_t *cfg, const char *host,
                         const char *name) {
    if (!host || !host[0] || !name || !name[0])
        return;
    for (int i = 0; i < cfg->pc_count; i++) {
        if (!strcasecmp(cfg->pcs[i].host, host)) {
            snprintf(cfg->pcs[i].name, sizeof(cfg->pcs[i].name), "%s", name);
            return;
        }
    }
}

/* Parse one "host | name | mac | auto" [pcs] line. Fields may be empty. */
static void parse_pc_line(app_config_t *cfg, const char *val) {
    char host[CONFIG_MAX_HOST] = "";
    char name[64] = "";
    char mac[24] = "";
    char auto_v[8] = "";
    char *fields[4] = { host, name, mac, auto_v };
    size_t caps[4] = { sizeof(host), sizeof(name), sizeof(mac),
                       sizeof(auto_v) };
    int fi = 0;
    const char *p = val;
    while (fi < 4) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len >= caps[fi])
            len = caps[fi] - 1;
        memcpy(fields[fi], p, len);
        fields[fi][len] = '\0';
        /* trim */
        char *d = fields[fi];
        while (isspace((unsigned char)*d))
            memmove(d, d + 1, strlen(d));
        for (char *e = d + strlen(d); e > d && isspace((unsigned char)e[-1]);)
            *--e = '\0';
        if (!bar)
            break;
        p = bar + 1;
        fi++;
    }
    if (!host[0])
        return;
    /* Append preserving file order (MRU); duplicates keep the first. */
    for (int i = 0; i < cfg->pc_count; i++) {
        if (!strcasecmp(cfg->pcs[i].host, host))
            return;
    }
    if (cfg->pc_count >= CONFIG_MAX_PCS)
        return;
    saved_pc_t *pc = &cfg->pcs[cfg->pc_count++];
    memset(pc, 0, sizeof(*pc));
    snprintf(pc->host, sizeof(pc->host), "%s", host);
    snprintf(pc->name, sizeof(pc->name), "%s", name);
    snprintf(pc->mac, sizeof(pc->mac), "%s", mac);
    pc->mac_auto = (char)(parse_bool(auto_v) ? 1 : 0);
}

int config_load(app_config_t *cfg, const char *dir) {
    char path[512];
    char line[512];

    config_set_defaults(cfg);
    LOGI("config_load: dir=%s", dir);

    // Compat: host.txt / debug_host.txt from phases 0-2
    snprintf(path, sizeof(path), "%s/host.txt", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(cfg->host, sizeof(cfg->host), f))
            cfg->host[strcspn(cfg->host, " \r\n")] = '\0';
        fclose(f);
        LOGI("config: host.txt => '%s'", cfg->host);
    }
    snprintf(path, sizeof(path), "%s/debug_host.txt", dir);
    f = fopen(path, "r");
    if (f) {
        if (fgets(cfg->debug_host, sizeof(cfg->debug_host), f))
            cfg->debug_host[strcspn(cfg->debug_host, " \r\n")] = '\0';
        fclose(f);
        LOGI("config: debug_host.txt => '%s'", cfg->debug_host);
    }

    snprintf(path, sizeof(path), "%s/moonlight.ini", dir);
    f = fopen(path, "r");
    if (!f) {
        LOGW("config: no hay %s", path);
        return cfg->host[0] ? 0 : -1;
    }
    LOGI("config: leyendo %s", path);

    int cfg_rev = 1; /* files without the key predate the rev-2 decode defaults */
    int surround_explicit = 0;
    char section[24] = ""; /* current [section]; "" = the flat legacy head */
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            /* [section] header - track it so the [pcs] keys below do not
             * collide with flat keys (a bare "count" used to warn). */
            char *close = strchr(p, ']');
            if (close) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", p + 1);
            }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (!strcmp(section, "pcs")) {
            /* Saved-PC list (MRU order = file order). */
            if (!strncmp(key, "pc", 2) && isdigit((unsigned char)key[2]) &&
                !key[3]) {
                parse_pc_line(cfg, val);
            } else if (!strcmp(key, "pc_active")) {
                int idx = atoi(val);
                if (idx >= 0 && idx < CONFIG_MAX_PCS)
                    cfg->pc_active = idx;
            }
            /* pc_count is informational; the lines themselves are the
             * source of truth (a truncated tail must not corrupt MRU). */
            continue;
        }

        if (!strcmp(key, "cfg_rev"))
            cfg_rev = atoi(val);
        else if (!strcmp(key, "host"))
            snprintf(cfg->host, sizeof(cfg->host), "%s", val);
        else if (!strcmp(key, "http_port")) {
            int port = atoi(val);
            if (port > 0 && port <= 65535) cfg->http_port = (unsigned short)port;
        }
        else if (!strcmp(key, "app"))
            snprintf(cfg->app_name, sizeof(cfg->app_name), "%s", val);
        else if (!strcmp(key, "debug_host"))
            snprintf(cfg->debug_host, sizeof(cfg->debug_host), "%s", val);
        else if (!strcmp(key, "width"))
            cfg->stream.width = atoi(val);
        else if (!strcmp(key, "height"))
            cfg->stream.height = atoi(val);
        else if (!strcmp(key, "fps"))
            cfg->stream.fps = atoi(val);
        else if (!strcmp(key, "bitrate"))
            cfg->stream.bitrate = atoi(val);
        else if (!strcmp(key, "packet_size"))
            cfg->stream.packetSize = atoi(val);
        else if (!strcmp(key, "sops"))
            cfg->sops = parse_bool(val);
        else if (!strcmp(key, "local_audio"))
            cfg->local_audio = parse_bool(val);
        else if (!strcmp(key, "prefer_hw"))
            cfg->prefer_hw = parse_bool(val);
        else if (!strcmp(key, "videodec2_spike"))
            cfg->videodec2_spike = parse_bool(val);
        else if (!strcmp(key, "prefer_ycbcr"))
            cfg->prefer_ycbcr = parse_bool(val);
        else if (!strcmp(key, "enable_file_log"))
            cfg->enable_file_log = parse_bool(val);
        else if (!strcmp(key, "show_stats"))
            cfg->show_stats = parse_bool(val);
        else if (!strcmp(key, "autostart"))
            cfg->autostart = parse_bool(val);
        else if (!strcmp(key, "ui_theme")) {
            int theme = atoi(val);
            cfg->ui_theme = theme >= 0 && theme < 6 ? theme : 5;
        }
        else if (!strcmp(key, "wol_mac")) {
            snprintf(cfg->wol_mac, sizeof(cfg->wol_mac), "%s", val);
        }
        else if (!strcmp(key, "wol_auto")) {
            cfg->wol_auto = parse_bool(val);
        }
        else if (!strcmp(key, "surround_mode")) {
            int mode = atoi(val);
            if (mode >= 0 && mode <= 2)
                cfg->surround_mode = mode;
            surround_explicit = 1;
        }
        else if (!strcmp(key, "surround51")) {
            /* legacy rev < 4 key */
            if (parse_bool(val))
                cfg->surround_mode = 1;
        }
        else if (!strcmp(key, "touchpad_mouse"))
            cfg->touchpad_mouse = parse_bool(val);
        else if (!strcmp(key, "gyro_motion"))
            cfg->gyro_motion = parse_bool(val);
        else if (!strcmp(key, "dec_pipeline_depth"))
            cfg->dec_pipeline_depth = atoi(val);
        else if (!strcmp(key, "dec_thread_prio"))
            cfg->dec_thread_prio = atoi(val);
        else if (!strcmp(key, "slices_per_frame"))
            cfg->slices_per_frame = atoi(val);
        else if (!strcmp(key, "dec_au_onion"))
            cfg->dec_au_onion = parse_bool(val);
        else if (!strcmp(key, "dec_fb_garlic"))
            cfg->dec_fb_garlic = parse_bool(val);
        else if (!strcmp(key, "bgra_workers"))
            cfg->bgra_workers = atoi(val);
        else if (!strcmp(key, "bgra_nt"))
            cfg->bgra_nt = atoi(val);
        else
            LOGW("config: clave desconocida '%s'", key);
    }
    fclose(f);

    /*
     * One-shot migration: rev < 2 files were written when the decode-tuning
     * defaults were depth=1 + AU in WC_GARLIC, and config_save pins every
     * key, so a plain default change would never reach existing installs.
     * Rev 2 defaults (validated as the fast path): pipelined Decode (depth=2,
     * overlaps CPU parse with HW decode; +1 frame of latency) and the AU
     * bitstream in cacheable ONION. Explicit user overrides survive from
     * rev >= 2 onward (the file carries cfg_rev = 2 after the next save).
     */
    if (cfg_rev < 2) {
        LOGW("config: rev %d < 2; aplicando nuevos defaults de decode "
             "(depth %d→2, au_onion %d→1)",
             cfg_rev, cfg->dec_pipeline_depth, cfg->dec_au_onion ? 1 : 0);
        cfg->dec_pipeline_depth = 2;
        cfg->dec_au_onion = true;
    }

    /* Rev 3: the Orbis theme (PS4 system look) ships as the new
     * default. Configs from earlier revisions always pin ui_theme, and
     * everyone who never touched the setting has 0 (the old default), so
     * rev < 3 files move a still-default 0 to Orbis once; deliberate
     * Midnight users can pick it back in Settings and rev 3 files keep
     * whatever they pin. */
    if (cfg_rev < 3 && cfg->ui_theme == 0) {
        cfg->ui_theme = 5;
        LOGW("config: rev %d < 3; theme 0 -> 5 (Orbis) one-time migration",
             cfg_rev);
    }

    /* Rev 4: the saved-PC list ([pcs]) and the surround tri-state arrive
     * together. A file with no [pcs] lines but a legacy flat host (or a
     * phases-0-2 host.txt) seeds the list with that one computer - its
     * wol_mac/wol_auto become the entry's per-PC MAC - so the PCs tab
     * opens with the user's machine already on it, exactly as before. */
    if (cfg->pc_count == 0 && cfg->host[0]) {
        saved_pc_t *pc = &cfg->pcs[0];
        memset(pc, 0, sizeof(*pc));
        snprintf(pc->host, sizeof(pc->host), "%s", cfg->host);
        snprintf(pc->mac, sizeof(pc->mac), "%s", cfg->wol_mac);
        pc->mac_auto = cfg->wol_auto ? 1 : 0;
        cfg->pc_count = 1;
        cfg->pc_active = 0;
        if (cfg_rev < 4)
            LOGW("config: rev %d < 4; migrating host '%s' into the saved "
                 "PC list", cfg_rev, cfg->host);
    }
    if (cfg->pc_active < 0 || cfg->pc_active >= cfg->pc_count)
        cfg->pc_active = cfg->pc_count > 0 ? 0 : -1;
    /* The [pcs] section is the store; the flat keys mirror the active
     * entry for every pre-list reader (stream.c, the launch ladder). */
    if (cfg->pc_count > 0) {
        const saved_pc_t *pc = &cfg->pcs[cfg->pc_active];
        snprintf(cfg->host, sizeof(cfg->host), "%s", pc->host);
        snprintf(cfg->wol_mac, sizeof(cfg->wol_mac), "%s", pc->mac);
        cfg->wol_auto = pc->mac_auto != 0;
    }

    LOGI("config: host=%s app=%s debug=%s %dx%d@%d br=%d pkt=%d hw=%d ycbcr=%d file_log=%d",
         cfg->host, cfg->app_name, cfg->debug_host,
         cfg->stream.width, cfg->stream.height, cfg->stream.fps, cfg->stream.bitrate,
         cfg->stream.packetSize, cfg->prefer_hw, cfg->prefer_ycbcr,
         cfg->enable_file_log);
    if (cfg->pc_count > 0)
        LOGI("config: %d saved PC(s), active=%d (%s)", cfg->pc_count,
             cfg->pc_active, cfg->pcs[cfg->pc_active].host);
    return 0;
}

int config_save(const app_config_t *cfg, const char *dir) {
    char path[512];
    config_ensure_dir(dir);
    snprintf(path, sizeof(path), "%s/moonlight.ini", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOGE("config: could not write %s", path);
        return -1;
    }
    fprintf(f,
            "# moonlight-ps4\n"
            "# host/wol_mac mirror the ACTIVE PC from [pcs] (MRU-first).\n"
            "cfg_rev = 4\n"
            "host = %s\n"
            "http_port = %u\n"
            "app = %s\n"
            "debug_host = %s\n"
            "width = %d\n"
            "height = %d\n"
            "fps = %d\n"
            "bitrate = %d\n"
            "packet_size = %d\n"
            "sops = %s\n"
            "local_audio = %s\n"
            "prefer_hw = %s\n"
            "videodec2_spike = %s\n"
            "prefer_ycbcr = %s\n"
            "enable_file_log = %s\n"
            "show_stats = %s\n"
            "autostart = %s\n"
            "ui_theme = %d\n"
            "wol_mac = %s\n"
            "wol_auto = %s\n"
            "surround_mode = %d\n"
            "touchpad_mouse = %s\n"
            "gyro_motion = %s\n"
            "dec_pipeline_depth = %d\n"
            "dec_thread_prio = %d\n"
            "slices_per_frame = %d\n"
            "dec_au_onion = %s\n"
            "dec_fb_garlic = %s\n"
            "bgra_workers = %d\n"
            "bgra_nt = %d\n",
            cfg->host, (unsigned)cfg->http_port, cfg->app_name, cfg->debug_host,
            cfg->stream.width, cfg->stream.height, cfg->stream.fps, cfg->stream.bitrate,
            cfg->stream.packetSize,
            cfg->sops ? "true" : "false",
            cfg->local_audio ? "true" : "false",
            cfg->prefer_hw ? "true" : "false",
            cfg->videodec2_spike ? "true" : "false",
            cfg->prefer_ycbcr ? "true" : "false",
            cfg->enable_file_log ? "true" : "false",
            cfg->show_stats ? "true" : "false",
            cfg->autostart ? "true" : "false",
            cfg->ui_theme,
            cfg->wol_mac,
            cfg->wol_auto ? "true" : "false",
            cfg->surround_mode,
            cfg->touchpad_mouse ? "true" : "false",
            cfg->gyro_motion ? "true" : "false",
            cfg->dec_pipeline_depth, cfg->dec_thread_prio, cfg->slices_per_frame,
            cfg->dec_au_onion ? "true" : "false",
            cfg->dec_fb_garlic ? "true" : "false",
            cfg->bgra_workers, cfg->bgra_nt);

    /* Saved-PC list: MRU-first; pc0 is the active computer. The pipe
     * format keeps hand-editing sane (empty fields are allowed). */
    if (cfg->pc_count > 0) {
        int active = cfg->pc_active >= 0 && cfg->pc_active < cfg->pc_count
                         ? cfg->pc_active : 0;
        fprintf(f, "\n[pcs]\npc_count = %d\npc_active = %d\n",
                cfg->pc_count, active);
        for (int i = 0; i < cfg->pc_count; i++) {
            const saved_pc_t *pc = &cfg->pcs[i];
            fprintf(f, "pc%d = %s | %s | %s | %s\n", i, pc->host,
                    pc->name[0] ? pc->name : "",
                    pc->mac[0] ? pc->mac : "",
                    pc->mac_auto ? "true" : "false");
        }
    }
    fclose(f);
    return 0;
}

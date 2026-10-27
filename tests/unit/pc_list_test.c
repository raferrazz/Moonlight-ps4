/* config.c saved-PC list unit tests - the [pcs] section is the store the
 * PCs tab renders, so its invariants (MRU order, active mirror, migration
 * from the legacy single-host file, cap + eviction, per-PC MACs) get a
 * host-side unit suite of their own. */
#include "../../src/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed;

static void expect(const char *what, long got, long want) {
    if (got != want) {
        printf("FAIL: %s: got %ld want %ld\n", what, got, want);
        g_failed++;
    } else {
        printf("ok: %s\n", what);
    }
}

static void expect_str(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        printf("FAIL: %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want);
        g_failed++;
    } else {
        printf("ok: %s\n", what);
    }
}

static void write_file(const char *dir, const char *name,
                       const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static char g_dir[256];

/* Link stub: config_set_defaults() runs the common-c initializer on the
 * console; the host suite only needs the symbol (the defaults after it
 * are plain struct writes, and this test never asserts stream fields). */
void LiInitializeStreamConfiguration(STREAM_CONFIGURATION *s) { (void)s; }

static const char *fresh_dir(void) {
    static int n;
    snprintf(g_dir, sizeof(g_dir), "/tmp/pc_list_test_%d_%d",
             (int)getpid(), n++);
    mkdir(g_dir, 0755);
    return g_dir;
}

int main(void) {
    /* 1. Legacy single-host file migrates transparently into the list. */
    {
        const char *dir = fresh_dir();
        write_file(dir, "moonlight.ini",
                   "cfg_rev = 3\n"
                   "host = 192.168.2.99\n"
                   "wol_mac = AA:BB:CC:DD:EE:FF\n"
                   "wol_auto = true\n"
                   "surround51 = true\n");
        app_config_t cfg;
        expect("legacy: load ok", config_load(&cfg, dir), 0);
        expect("legacy: one PC", cfg.pc_count, 1);
        expect("legacy: active 0", cfg.pc_active, 0);
        expect_str("legacy: host mirrored", cfg.host, "192.168.2.99");
        expect_str("legacy: mac mirrored", cfg.wol_mac, "AA:BB:CC:DD:EE:FF");
        expect("legacy: mac auto", cfg.wol_auto, 1);
        expect("legacy: surround51 -> mode 1", cfg.surround_mode, 1);
        expect_str("legacy: pc host", cfg.pcs[0].host, "192.168.2.99");
        expect_str("legacy: pc mac", cfg.pcs[0].mac, "AA:BB:CC:DD:EE:FF");
    }

    /* 2. Save/load round trip keeps the list and the active PC. */
    {
        const char *dir = fresh_dir();
        app_config_t cfg;
        config_set_defaults(&cfg);
        config_pc_add(&cfg, "10.0.0.5", "STUDY", "11:22:33:44:55:66");
        config_pc_add(&cfg, "10.0.0.7", "LIVING", "");
        expect("roundtrip: add second active", cfg.pc_active, 0);
        expect_str("roundtrip: host is new head", cfg.host, "10.0.0.7");
        expect("roundtrip: save", config_save(&cfg, dir), 0);

        app_config_t back;
        expect("roundtrip: load", config_load(&back, dir), 0);
        expect("roundtrip: count", back.pc_count, 2);
        expect("roundtrip: active", back.pc_active, 0);
        expect_str("roundtrip: pc0 host", back.pcs[0].host, "10.0.0.7");
        expect_str("roundtrip: pc0 name", back.pcs[0].name, "LIVING");
        expect_str("roundtrip: pc0 mac empty", back.pcs[0].mac, "");
        expect_str("roundtrip: pc1 host", back.pcs[1].host, "10.0.0.5");
        expect_str("roundtrip: pc1 name", back.pcs[1].name, "STUDY");
        expect_str("roundtrip: pc1 mac", back.pcs[1].mac,
                    "11:22:33:44:55:66");
        expect_str("roundtrip: flat host", back.host, "10.0.0.7");
        expect_str("roundtrip: no mac mirror", back.wol_mac, "");
    }

    /* 3. Touch: selecting a PC moves it to the MRU head + mirrors. */
    {
        app_config_t cfg;
        config_set_defaults(&cfg);
        config_pc_add(&cfg, "10.0.0.5", "A", "AA:AA:AA:AA:AA:AA");
        config_pc_add(&cfg, "10.0.0.7", "B", ""); /* order: [B, A] */
        config_pc_touch(&cfg, 1); /* select A -> order: [A, B] */
        expect_str("touch: head is A", cfg.pcs[0].host, "10.0.0.5");
        expect_str("touch: tail is B", cfg.pcs[1].host, "10.0.0.7");
        expect("touch: active 0", cfg.pc_active, 0);
        expect_str("touch: host mirror", cfg.host, "10.0.0.5");
        expect_str("touch: mac mirror follows", cfg.wol_mac,
                    "AA:AA:AA:AA:AA:AA");
        config_pc_touch(&cfg, 1); /* select B -> order: [B, A] */
        expect_str("touch: head is B again", cfg.pcs[0].host, "10.0.0.7");
        expect_str("touch: mac mirror swaps too", cfg.wol_mac, "");
        expect("touch: B stays not-auto", cfg.wol_auto, 0);
    }

    /* 4. Cap: the 9th PC evicts the LRU tail; duplicates re-select. */
    {
        app_config_t cfg;
        config_set_defaults(&cfg);
        for (int i = 1; i <= 8; i++) {
            char host[32];
            snprintf(host, sizeof(host), "10.0.0.%d", i);
            config_pc_add(&cfg, host, "", "");
        }
        expect("cap: filled to 8", cfg.pc_count, 8);
        expect_str("cap: head is 8th", cfg.pcs[0].host, "10.0.0.8");
        config_pc_add(&cfg, "10.0.0.9", "", "");
        expect("cap: still 8", cfg.pc_count, 8);
        expect_str("cap: head is 9th", cfg.pcs[0].host, "10.0.0.9");
        expect_str("cap: tail evicted LRU (was 1st)",
                   cfg.pcs[7].host, "10.0.0.2");
        config_pc_add(&cfg, "10.0.0.4", "", ""); /* duplicate */
        expect("cap: dup keeps count", cfg.pc_count, 8);
        expect_str("cap: dup moved to head", cfg.pcs[0].host, "10.0.0.4");
        expect("cap: dup active", cfg.pc_active, 0);
    }

    /* 5. Remove: shifting, active fix-up, and emptying the list. */
    {
        app_config_t cfg;
        config_set_defaults(&cfg);
        config_pc_add(&cfg, "10.0.0.5", "", "");
        config_pc_add(&cfg, "10.0.0.6", "", "");
        config_pc_add(&cfg, "10.0.0.7", "", ""); /* active head */
        config_pc_remove(&cfg, 0);                /* forget the head */
        expect("remove: count 2", cfg.pc_count, 2);
        expect("remove: active follows new head", cfg.pc_active, 0);
        expect_str("remove: host mirror", cfg.host, "10.0.0.6");
        config_pc_remove(&cfg, 1);
        config_pc_remove(&cfg, 0);
        expect("remove: emptied", cfg.pc_count, 0);
        expect("remove: no active", cfg.pc_active, -1);
        expect_str("remove: host cleared", cfg.host, "");
        expect_str("remove: mac cleared", cfg.wol_mac, "");
    }

    /* 6. Per-PC MACs: note (auto) vs manual, name updates. */
    {
        app_config_t cfg;
        config_set_defaults(&cfg);
        config_pc_add(&cfg, "10.0.0.5", "", "");
        config_pc_add(&cfg, "10.0.0.6", "", "");
        config_pc_touch(&cfg, 1); /* active = 10.0.0.5 (head) */
        config_pc_note_mac(&cfg, "CC:DD:EE:00:11:22");
        expect_str("mac: stored on active", cfg.pcs[0].mac,
                    "CC:DD:EE:00:11:22");
        expect("mac: auto flag", cfg.pcs[0].mac_auto, 1);
        expect_str("mac: mirror", cfg.wol_mac, "CC:DD:EE:00:11:22");
        expect("mac: wol_auto", cfg.wol_auto, 1);
        config_pc_set_mac_manual(&cfg, "99:88:77:66:55:44");
        expect("mac: manual flag", cfg.pcs[0].mac_auto, 0);
        expect("mac: wol_auto cleared", cfg.wol_auto, 0);
        expect_str("mac: manual value kept", cfg.wol_mac,
                    "99:88:77:66:55:44");
        /* The other PC keeps its (empty) MAC untouched. */
        expect_str("mac: other PC untouched", cfg.pcs[1].mac, "");
        config_pc_note_name(&cfg, "10.0.0.5", "BASEMENT");
        expect_str("mac: name noted", cfg.pcs[0].name, "BASEMENT");
    }

    /* 7. A hand-written [pcs] section parses (spaces, empty fields). */
    {
        const char *dir = fresh_dir();
        write_file(dir, "moonlight.ini",
                   "cfg_rev = 4\n"
                   "host = 10.0.0.2\n"
                   "[pcs]\n"
                   "pc_count = 3\n"
                   "pc_active = 2\n"
                   "pc0 = 10.0.0.2 | STUDY | 11:11:11:11:11:11 | true\n"
                   "pc1 = 10.0.0.3 |  |  | false\n"
                   "pc2 = 10.0.0.4 | GAMING\n");
        app_config_t cfg;
        expect("handfile: load", config_load(&cfg, dir), 0);
        expect("handfile: count", cfg.pc_count, 3);
        expect("handfile: active", cfg.pc_active, 2);
        expect_str("handfile: host mirror", cfg.host, "10.0.0.4");
        expect_str("handfile: name w/o tail fields", cfg.pcs[2].name,
                    "GAMING");
        expect_str("handfile: empty fields ok", cfg.pcs[1].mac, "");
        expect_str("handfile: auto flag", cfg.pcs[0].mac, "11:11:11:11:11:11");
        expect("handfile: auto true", cfg.pcs[0].mac_auto, 1);
    }

    /* 8. surround tri-state round trip. */
    {
        const char *dir = fresh_dir();
        app_config_t cfg;
        config_set_defaults(&cfg);
        config_pc_add(&cfg, "10.0.0.5", "", "");
        cfg.surround_mode = 2;
        expect("surround: save", config_save(&cfg, dir), 0);
        app_config_t back;
        expect("surround: load", config_load(&back, dir), 0);
        expect("surround: mode kept", back.surround_mode, 2);
    }

    if (g_failed) {
        printf("== %d pc_list test(s) failed ==\n", g_failed);
        return 1;
    }
    printf("== all pc_list tests passed ==\n");
    return 0;
}

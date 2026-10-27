/* Wake-on-LAN unit tests: MAC parse/format round trips, the 102-byte
 * magic-packet layout, and the validation rules the Settings keyboard and
 * the PCs tab rely on. */
#include "../../src/gamestream/discovery.h"

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

int main(void) {
    uint8_t mac[6], out[102];

    /* 1. Colon-separated parse. */
    expect("parse AA:BB:CC:DD:EE:FF",
           wol_mac_parse("AA:BB:CC:DD:EE:FF", mac), 0);
    expect("octet 0", mac[0], 0xAA);
    expect("octet 5", mac[5], 0xFF);

    /* 2. Dash-separated and no-separator forms. */
    expect("parse dash form", wol_mac_parse("3C-97-0E-12-34-56", mac), 0);
    expect("dash octet", mac[0], 0x3C);
    expect("parse glued form", wol_mac_parse("3c970e123456", mac), 0);
    expect("glued octet 2", mac[2], 0x0E);

    /* 3. Case-insensitivity. */
    expect("lowercase parse", wol_mac_parse("aa:bb:cc:dd:ee:ff", mac), 0);
    expect("mixed case octet", mac[1], 0xBB);

    /* 4. Rejections: wrong count, bad hex, garbage, empty. */
    expect("reject 11 digits", wol_mac_parse("AABBCCDDEEF", mac) != 0, 1);
    expect("reject 13 digits", wol_mac_parse("AABBCCDDEEFF0", mac) != 0, 1);
    expect("reject non-hex", wol_mac_parse("AA:BB:CC:DD:EE:GG", mac) != 0, 1);
    expect("reject empty", wol_mac_parse("", mac) != 0, 1);

    /* 5. Format round trip. */
    uint8_t src[6] = { 0x3C, 0x97, 0x0E, 0x12, 0x34, 0x56 };
    char buf[24];
    wol_mac_format(src, buf, sizeof(buf));
    expect("format", strcmp(buf, "3C:97:0E:12:34:56") == 0, 1);
    uint8_t back[6];
    expect("round trip", wol_mac_parse(buf, back), 0);
    expect("round trip octets", memcmp(src, back, 6) == 0, 1);

    /* 6. Magic packet layout: 6x 0xFF then 16x the MAC = 102 bytes. */
    wol_build_packet(src, out);
    int ff_ok = 1;
    for (int i = 0; i < 6; i++)
        if (out[i] != 0xFF)
            ff_ok = 0;
    expect("packet: six 0xFF", ff_ok, 1);
    int mac_ok = 1;
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 6; i++)
            if (out[6 + k * 6 + i] != src[i])
                mac_ok = 0;
    expect("packet: 16x MAC", mac_ok, 1);

    /* 7. IPv4 validation used by the PCs/Settings flows. */
    expect("valid ipv4", wol_mac_parse ? (mdns_valid_ipv4("192.168.1.42") == 1) : 0, 1);
    expect("valid ipv4 loopback", mdns_valid_ipv4("127.0.0.1") == 1, 1);
    expect("reject 4 fields", mdns_valid_ipv4("192.168.1") == 0, 1);
    expect("reject alpha", mdns_valid_ipv4("192.168.one.1") == 0, 1);
    expect("reject >255", mdns_valid_ipv4("192.168.1.300") == 0, 1);
    expect("reject negative-ish", mdns_valid_ipv4("192.168.1.-4") == 0, 1);

    if (g_failed) {
        printf("== %d wol test(s) failed ==\n", g_failed);
        return 1;
    }
    printf("== all wol tests passed ==\n");
    return 0;
}

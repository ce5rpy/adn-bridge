/*
 * Wire-level unit checks for session/media helpers.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "media/call_meta.h"
#include "media/identity.h"
#include "session/dmr_wire.h"
#include "session/ysf_tx.h"

static void test_dmr_wire(void)
{
    uint8_t rf[3];
    uint8_t vhead[55];
    uint8_t vterm[55];

    memset(vhead, 0, sizeof(vhead));
    memcpy(vhead, "DMRD", 4);
    vhead[15] = (uint8_t)((DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD);
    assert(dmrd_is_header(vhead, 55));
    assert(!dmrd_is_voice(vhead, 55));

    memset(vterm, 0, sizeof(vterm));
    memcpy(vterm, "DMRD", 4);
    vterm[15] = (uint8_t)((DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM);
    assert(dmrd_is_terminator(vterm, 55));

    dmr_id_to_bytes3(1234567, rf);
    assert(rf[0] == 0x12 && rf[1] == 0xD6 && rf[2] == 0x87);
    assert(dmr_id_rf24(123456789) == 1234567);

    {
        uint8_t pong[11] = {'M', 'S', 'T', 'P', 'O', 'N', 'G', 0x00, 0x0b, 0x24, 0x3a};
        uint32_t id = 0;
        char hex[32];

        assert(strcmp(hbp_cmd_label(pong, 11), "MSTPONG") == 0);
        assert(hbp_wire_tail_id(pong, 11, "MSTPONG", &id));
        assert(id == 730170U);
        hbp_wire_hex(pong, 11, hex, sizeof(hex));
        assert(strcmp(hex, "4d5354504f4e47000b243a") == 0);
    }
}

static void test_identity_callsign(void)
{
    char base[16];
    char out[10];
    char pad[10];
    uint8_t numeric[10];

    memcpy(pad, "HP3ICC-FT ", 10);
    memcpy(numeric, "  1234567 ", 10);

    identity_callsign_base10(pad, base);
    assert(strcmp(base, "HP3ICC") == 0);

    identity_format_base_callsign10(out, "ca5rpy-l");
    assert(memcmp(out, "CA5RPY    ", 10) == 0);

    identity_format_full_callsign10(out, "ca5rpy-l");
    assert(memcmp(out, "CA5RPY-L  ", 10) == 0);

    assert(identity_callsign10_to_dmrid(numeric) == 1234567);
}

/* A YSF->YSF relay must keep the source radio's real callsign (incl. any
 * -SUFFIX/SUFFIX after '-' or '/') intact -- only the DMR ID/alias lookup
 * needs the truncated base form, since a DMR radio ID has no room for it. */
static void test_identity_resolve_ysf_header_keeps_full_callsign(void)
{
    uint8_t pkt[155];
    bridge_call_meta_t meta;
    identity_ysf_ctx_t ctx = { NULL, 0, NULL };
    int rf_id = 0;

    memset(pkt, 0, sizeof(pkt));
    memcpy(pkt, "YSFD", 4);
    /* Garbage FICH/CSD payload -> ysf_payload_process_header fails on both
     * try offsets, forcing the raw wire_src_fallback path (reads pkt+14
     * directly), same as a real malformed/late-join frame would. */
    memcpy(pkt + 14, "1234567-A ", 10);

    bridge_call_meta_clear(&meta);
    assert(identity_resolve_ysf_header(&meta, &rf_id, &ctx, pkt) == 1);
    /* Base "1234567" is numeric -> resolves via identity_callsign10_to_dmrid
     * alone, no alias DB needed -- keeps this test self-contained. */
    assert(rf_id == 1234567);
    assert(memcmp(meta.net_src, "1234567-A ", 10) == 0);
}

static void test_ysf_csd(void)
{
    bridge_call_meta_t meta;
    uint8_t csd1[20];
    uint8_t csd2[20];

    bridge_call_meta_clear(&meta);
    memcpy(meta.net_src, "CE5RPY    ", 10);
    ysf_tx_fill_csd(&meta, csd1, csd2);
    assert(memcmp(csd1, "*****", 5) == 0);
    assert(memcmp(csd1 + 10, meta.net_src, 10) == 0);
    assert(memcmp(csd2, "                    ", 20) == 0);
}

int main(void)
{
    test_dmr_wire();
    test_identity_callsign();
    test_identity_resolve_ysf_header_keeps_full_callsign();
    test_ysf_csd();
    printf("test_wire: ok\n");
    return 0;
}

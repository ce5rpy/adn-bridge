/*
 * YSF <-> DMR voice bridge (ModeConv pacing and framing).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "bridge.h"
#include "hbp/dmr_codec.h"
#include "log.h"
#include "aliases.h"
#include "mmdvm/modeconv_wrap.h"
#include "mmdvm/ysfpayload_wrap.h"
#include "ysf_fich.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DMR_FRAME_MS  55
#define YSF_FRAME_MS  90
#define YSF_DT_VD_MODE1 0x00U
#define YSF_DT_VD_MODE2 0x02U
#define YSF_DT_VOICE_FR 0x03U
#define YSF_FI_HEADER         0x00U
#define YSF_FI_COMMUNICATIONS 0x01U
#define YSF_FI_TERMINATOR     0x02U
#define YSF_FICH_FT           6U   /* DMR2YSF FICHFrameTotal=6 → fn 0..6 */
#define YSF_FICH_CM           0U   /* YSF2DMR default call mode */
#define YSF_WIRE_DST_ALL      "ALL       "
#define YSF_SYNC_BYTES        "\xD4\x71\xC9\x63\x4D"

/* HBP DMRD byte 15 — matches new-adn-server parse_dmrd_burst_fields */
#define DMRD_FT_DATA_SYNC 2U
#define DMRD_FT_VOICE_SYNC 1U
#define DMRD_DTYPE_VHEAD  1U
#define DMRD_DTYPE_VTERM  2U

/* DMR burst sync field (bytes 13..19 of the 33-byte payload) — DMRDefines.h */
static const uint8_t MS_SOURCED_AUDIO_SYNC[7] =
    {0x07U, 0xF7U, 0xD5U, 0xDDU, 0x57U, 0xDFU, 0xD0U};
static const uint8_t DMR_SYNC_MASK[7] =
    {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U};
static const uint8_t DMR_SILENCE_DATA[33] =
    {0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U,
     0x81U, 0x52U, 0x60U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x73U, 0x00U,
     0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU};

static void dbg_label10(char out[11], const uint8_t raw[10])
{
    int i;

    for (i = 0; i < 10; i++)
        out[i] = (raw[i] >= 32 && raw[i] < 127) ? (char)raw[i] : '.';
    out[10] = '\0';
}

static int dbg_periodic(int *n)
{
    (*n)++;
    return (*n <= 2 || (*n % 20) == 0);
}

static const char *modeconv_tag_name(unsigned int tag)
{
    switch (tag) {
    case MODECONV_TAG_HEADER: return "HEADER";
    case MODECONV_TAG_DATA:   return "DATA";
    case MODECONV_TAG_EOT:    return "EOT";
    default:                  return "NODATA";
    }
}

static const char *ysf_fi_name(uint8_t fi)
{
    switch (fi) {
    case YSF_FI_HEADER:         return "HDR";
    case YSF_FI_COMMUNICATIONS: return "VOICE";
    case YSF_FI_TERMINATOR:     return "EOT";
    default:                  return "?";
    }
}

static uint8_t dmr_slot_bit_from_options(const char *options)
{
    if (options && strstr(options, "TS1="))
        return 0x00;
    return 0x80; /* TS2 default */
}

static void dmrd_parse_b15(uint8_t b15, uint8_t *ft, uint8_t *dtype)
{
    *ft = (b15 >> 4) & 0x03U;
    *dtype = b15 & 0x0fU;
}

static int ms_elapsed(const struct timespec *since, int interval_ms)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - since->tv_sec) * 1000L
                 + (now.tv_nsec - since->tv_nsec) / 1000000L;
    return elapsed >= interval_ms;
}

static void stamp_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static uint32_t new_stream_id(void)
{
    static int seeded;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    return (uint32_t)rand() | 1U;
}

static int dmr_id_rf24(int dmrid)
{
    return (dmrid > 99999999) ? dmrid / 100 : dmrid;
}

static void dmr_id_to_bytes3(int dmrid, uint8_t out[3])
{
    int id = dmr_id_rf24(dmrid);

    out[0] = (uint8_t)((id >> 16) & 0xff);
    out[1] = (uint8_t)((id >> 8) & 0xff);
    out[2] = (uint8_t)(id & 0xff);
}

static void format_id_callsign10(char out[10], int id)
{
    char tmp[12];
    int n;

    memset(out, ' ', 10);
    snprintf(tmp, sizeof(tmp), "%d", id);
    n = (int)strlen(tmp);
    if (n > 10)
        n = 10;
    memcpy(out, tmp, (size_t)n);
}

static void format_tg_dst10(char out[10], int tg)
{
    char tmp[16];
    int n;

    memset(out, ' ', 10);
    snprintf(tmp, sizeof(tmp), "TG %d", tg);
    n = (int)strlen(tmp);
    if (n > 10)
        n = 10;
    memcpy(out, tmp, (size_t)n);
}

static int label_is_numeric_dmrid(const uint8_t cs[10])
{
    int i, has_digit = 0;

    for (i = 0; i < 10; i++) {
        if (cs[i] == ' ')
            continue;
        if (cs[i] >= '0' && cs[i] <= '9')
            has_digit = 1;
        else
            return 0;
    }
    return has_digit;
}

static int callsign10_to_dmrid(const uint8_t cs[10])
{
    char digits[16];
    int i, j = 0;

    if (!label_is_numeric_dmrid(cs))
        return 0;

    for (i = 0; i < 10; i++) {
        if (cs[i] >= '0' && cs[i] <= '9')
            digits[j++] = (char)cs[i];
    }
    if (j == 0)
        return 0;
    digits[j] = '\0';
    return atoi(digits);
}

static void find_ysf_id_trim(const char *cs, char out[16])
{
    int first = -1, last = -1, mid1 = -1, mid2 = -1;
    int i, j = 0;

    for (i = 0; cs[i]; i++) {
        if (cs[i] != ' ') {
            if (first < 0)
                first = i;
            last = i;
        }
    }
    for (i = 0; cs[i]; i++) {
        if (cs[i] == '-')
            mid1 = i;
        if (cs[i] == '/')
            mid2 = i;
    }

    if (first < 0 || last < 0) {
        strncpy(out, "N0CALL", 15);
        out[15] = '\0';
        return;
    }
    if (mid1 < 0 && mid2 < 0) {
        for (i = first; i <= last && j < 15; i++)
            out[j++] = cs[i];
    } else if (mid1 > first) {
        for (i = first; i < mid1 && j < 15; i++)
            out[j++] = cs[i];
    } else if (mid2 > first) {
        for (i = first; i < mid2 && j < 15; i++)
            out[j++] = cs[i];
    } else {
        strncpy(out, "N0CALL", 15);
        j = 6;
    }
    out[j] = '\0';
}

static int ysf_pkt_has_rf_sync(const uint8_t *pkt155)
{
    static const uint8_t sync[5] = {0xD4U, 0x71U, 0xC9U, 0x63U, 0x4DU};
    return memcmp(pkt155 + YSF_FICH_OFFSET_NET, sync, 5) == 0;
}

/* Repack a network YSFD without leading sync into RF layout (sync+FICH+payload). */
static void ysf_net_to_rf120(const uint8_t *pkt155, uint8_t rf120[120])
{
    const uint8_t *net = pkt155 + YSF_FICH_OFFSET_NET;

    memset(rf120, 0, 120);
    memcpy(rf120, YSF_SYNC_BYTES, 5);
    memcpy(rf120 + 5, net, 115);
}

static const uint8_t *ysf_modeconv_chunk(const uint8_t *pkt155, uint8_t scratch[120])
{
    if (ysf_pkt_has_rf_sync(pkt155))
        return pkt155 + YSF_FICH_OFFSET_NET;
    ysf_net_to_rf120(pkt155, scratch);
    return scratch;
}


static int bridge_find_ysf_dmrid(ysf2dmr_bridge_t *b, const char src10[10])
{
    char trimmed[16];
    int id;

    id = callsign10_to_dmrid((const uint8_t *)src10);
    if (id > 0)
        return id;

    find_ysf_id_trim(src10, trimmed);
    if (b->aliases && trimmed[0])
        id = ysf2dmr_alias_lookup_id(b->aliases, trimmed);
    if (id > 0)
        return id;

    return b->default_ysf_dmrid;
}

static void radio_id_to_dch5(const char radio_id[6], uint8_t out[5])
{
    int i;

    memset(out, '*', 5);
    if (!radio_id || !radio_id[0])
        return;
    for (i = 0; i < 5; i++) {
        if (radio_id[i] == '\0')
            break;
        out[i] = (uint8_t)radio_id[i];
    }
}

static int bridge_assign_ysf_talker(ysf2dmr_bridge_t *b, const char src10[10])
{
    int id;

    memcpy(b->net_src, src10, 10);
    id = bridge_find_ysf_dmrid(b, src10);
    if (id <= 0 && b->default_ysf_dmrid > 0)
        id = b->default_ysf_dmrid;
    b->ysf_rf_id = id;
    return id > 0;
}

static int bridge_wire_src_fallback(ysf2dmr_bridge_t *b, const uint8_t *pkt)
{
    char wire[11];

    dbg_label10(wire, pkt + 14);
    return bridge_assign_ysf_talker(b, wire);
}

static int bridge_resolve_ysf_header(ysf2dmr_bridge_t *b, const uint8_t *pkt)
{
    uint8_t rf[120];
    uint8_t scratch[120];
    char csd_src[11], csd_dst[11];
    int i;
    const uint8_t *tries[2];

    tries[0] = ysf_modeconv_chunk(pkt, scratch);
    tries[1] = pkt + YSF_FICH_OFFSET_NET;

    for (i = 0; i < 2; i++) {
        memcpy(rf, tries[i], 120);
        if (!ysf_payload_process_header(rf, csd_src, csd_dst))
            continue;

        if (bridge_assign_ysf_talker(b, csd_src)) {
            LOG_DEBUG("YSF HEADER CSD src=%.10s dst=%.10s -> DMR id %d (try %d)\n",
                      csd_src, csd_dst, b->ysf_rf_id, i);
            return 1;
        }
        LOG_DEBUG("YSF HEADER CSD src=%.10s dst=%.10s (no DMR id, try %d)\n",
                  csd_src, csd_dst, i);
    }

    if (bridge_wire_src_fallback(b, pkt)) {
        LOG_DEBUG("YSF HEADER wire src=%.10s -> DMR id %d\n", b->net_src, b->ysf_rf_id);
        return 1;
    }

    if (b->default_ysf_dmrid > 0) {
        char wire[11];

        dbg_label10(wire, pkt + 14);
        memcpy(b->net_src, wire, 10);
        b->ysf_rf_id = b->default_ysf_dmrid;
        LOG_DEBUG("YSF HEADER wire src=%.10s -> default DMR id %d\n", b->net_src, b->ysf_rf_id);
        return 1;
    }

    LOG_DEBUG("YSF HEADER: no talker DMR id (set alias or default_ysf_dmrid)\n");
    return 0;
}

static int bridge_ysf_talker_ready(const ysf2dmr_bridge_t *b)
{
    return b->ysf_rf_id > 0 || b->default_ysf_dmrid > 0;
}

static void bridge_set_dmr_rx_identity(ysf2dmr_bridge_t *b, const uint8_t *pkt)
{
    int rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    int dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];

    if (!b->aliases || !ysf2dmr_alias_lookup_callsign(b->aliases, rf, b->net_src))
        format_id_callsign10(b->net_src, rf);
    format_tg_dst10(b->net_dst, dst);
}

static void bridge_reset_call(ysf2dmr_bridge_t *b)
{
    b->call_active = 0;
    b->dmr_voice_frames = 0;
    b->dmr_tx_frames = 0;
    b->dmr_dmrd_other = 0;
    b->ysf_voice_frames = 0;
    b->ysf_rf_id = 0;
    b->ysf_cnt = 0;
    memset(b->net_src, ' ', 10);
    memset(b->net_dst, ' ', 10);
    modeconv_reset();
}

void bridge_init(ysf2dmr_bridge_t *b, const char *dmr_options,
                 ysf2dmr_aliases_t *aliases, int default_ysf_dmrid,
                 const char *radio_id)
{
    memset(b, 0, sizeof(*b));
    b->aliases = aliases;
    b->default_ysf_dmrid = default_ysf_dmrid;
    memcpy(b->radio_id, "*****", 5);
    b->radio_id[5] = '\0';
    if (radio_id && radio_id[0])
        strncpy(b->radio_id, radio_id, sizeof(b->radio_id) - 1);
    b->radio_id[sizeof(b->radio_id) - 1] = '\0';
    b->dmr_slot_bit = dmr_slot_bit_from_options(dmr_options);
    memset(b->net_src, ' ', 10);
    memset(b->net_dst, ' ', 10);
    memcpy(b->net_dst, "ALL       ", 10);
    modeconv_init();
    stamp_now(&b->last_dmr_tx);
    stamp_now(&b->last_ysf_tx);
}

static int dmrd_is_voice(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    if (pkt[15] & 0x40)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft <= 1U;
}

static int dmrd_is_header(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    if (pkt[15] & 0x40)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft == DMRD_FT_DATA_SYNC && dtype == DMRD_DTYPE_VHEAD;
}

static int dmrd_is_terminator(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft == DMRD_FT_DATA_SYNC && dtype == DMRD_DTYPE_VTERM;
}

static const char *dmrd_class_label(const uint8_t *pkt, int len)
{
    if (dmrd_is_header(pkt, len))
        return "VHEAD";
    if (dmrd_is_terminator(pkt, len))
        return "VTERM";
    if (dmrd_is_voice(pkt, len))
        return "VOICE";
    return "OTHER";
}

static void bridge_send_dmrd(ysf2dmr_bridge_t *b, uint8_t frame_type, const uint8_t *voice33)
{
    uint8_t pkt[55];
    uint8_t rf[3];
    int rf_id;
    int src_id;

    if (b->ysf_rf_id > 0)
        rf_id = b->ysf_rf_id;
    else
        rf_id = b->default_ysf_dmrid;
    if (rf_id <= 0) {
        LOG_WARNING("DMR TX skipped: no YSF talker id (set default_ysf_dmrid or alias)\n");
        return;
    }
    src_id = dmr_id_rf24(rf_id);

    dmr_id_to_bytes3(rf_id, rf);

    memcpy(pkt, "DMRD", 4);
    pkt[4] = b->dmr_seq++;
    pkt[5] = rf[0];
    pkt[6] = rf[1];
    pkt[7] = rf[2];
    pkt[8] = (b->dmr.tg >> 16) & 0xff;
    pkt[9] = (b->dmr.tg >> 8) & 0xff;
    pkt[10] = (b->dmr.tg >> 0) & 0xff;
    pkt[11] = (b->dmr.dmrid >> 24) & 0xff;
    pkt[12] = (b->dmr.dmrid >> 16) & 0xff;
    pkt[13] = (b->dmr.dmrid >> 8) & 0xff;
    pkt[14] = (b->dmr.dmrid >> 0) & 0xff;
    pkt[15] = frame_type;
    *(uint32_t *)(pkt + 16) = b->dmr_stream_id;

    memcpy(buf, pkt, 55);
    rx_srcid = src_id;
    tx_tgid = b->dmr.tg;

    /* YSF2DMR reference frame construction:
     * - data sync (VHEAD/VTERM): full LC + slot type + MS data sync
     * - voice sync (n=0): AMBE + MS audio sync, refresh embedded LC
     * - voice (n=1..5): AMBE + embedded LC fragment + EMB */
    if (((frame_type >> 4) & 0x03U) == DMRD_FT_DATA_SYNC) {
        generate_header();
        memcpy(pkt + 20, buf + 20, 33);
    } else if (voice33) {
        int i;

        memcpy(pkt + 20, voice33, 33);
        if (((frame_type >> 4) & 0x03U) == DMRD_FT_VOICE_SYNC) {
            for (i = 0; i < 7; i++)
                pkt[20 + 13 + i] = (uint8_t)((pkt[20 + 13 + i] & ~DMR_SYNC_MASK[i])
                                             | MS_SOURCED_AUDIO_SYNC[i]);
            encode_embedded_data();
        } else {
            uint8_t lcss;

            memcpy(buf + 20, pkt + 20, 33);
            lcss = get_embedded_data(buf + 20, frame_type & 0x0f);
            get_emb_data(buf + 20, lcss);
            memcpy(pkt + 20, buf + 20, 33);
        }
    } else {
        memset(pkt + 20, 0, 33);
    }

    peer_dmr_send(&b->dmr, pkt, 55);
    stamp_now(&b->last_dmr_tx);
    {
        static int tx_log;
        uint8_t ft, dtype;
        dmrd_parse_b15(frame_type, &ft, &dtype);
        if (dbg_periodic(&tx_log)
            || (ft == DMRD_FT_DATA_SYNC
                && (dtype == DMRD_DTYPE_VHEAD || dtype == DMRD_DTYPE_VTERM))) {
            LOG_DEBUG("DMR TX %s b15=0x%02x rf=%d gw=%d tg=%d seq=%u stream=0x%08x\n",
                dmrd_class_label(pkt, 55), frame_type, src_id, b->dmr.dmrid,
                b->dmr.tg, (unsigned)pkt[4], (unsigned)b->dmr_stream_id);
        }
    }
}

static void bridge_emit_dmr_from_conv(ysf2dmr_bridge_t *b)
{
    uint8_t voice[33];
    unsigned int tag = modeconv_get_dmr(voice);
    uint8_t slot_bit = b->dmr_slot_bit;
    static int drain_log;

    if (tag == MODECONV_TAG_NODATA)
        return;

    if (dbg_periodic(&drain_log))
        LOG_DEBUG("ModeConv->DMR %s (tx_frames=%d ysf_in=%d)\n",
            modeconv_tag_name(tag), b->dmr_tx_frames, b->ysf_voice_frames);

    if (tag == MODECONV_TAG_HEADER) {
        int i;
        for (i = 0; i < 3; i++)
            bridge_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD), NULL);
        return;
    }
    if (tag == MODECONV_TAG_EOT) {
        /* YSF2DMR: pad with silence to the end of the 6-frame superframe. */
        while ((b->dmr_tx_frames % 6) != 0) {
            uint8_t n = (uint8_t)(b->dmr_tx_frames % 6);
            bridge_send_dmrd(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
            b->dmr_tx_frames++;
        }
        bridge_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM), voice);
        LOG_INFO("YSF->DMR call end (%d DMR frames out, talker %.10s)\n",
                b->dmr_tx_frames, b->net_src);
        bridge_reset_call(b);
        return;
    }
    if (tag == MODECONV_TAG_DATA) {
        /* n cycles 0..5 within the superframe; n=0 is the voice sync burst. */
        uint8_t n = (uint8_t)(b->dmr_tx_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        bridge_send_dmrd(b, b15, voice);
        b->dmr_tx_frames++;
    }
}

static void bridge_fill_ysf_csd(ysf2dmr_bridge_t *b, uint8_t csd1[20], uint8_t csd2[20])
{
    uint8_t rid[5];

    memset(csd1, 0, 20);
    memset(csd2, ' ', 20);
    memset(csd1, '*', 5);
    radio_id_to_dch5(b->radio_id, rid);
    memcpy(csd1 + 5, rid, 5);
    memcpy(csd1 + 10, b->net_src, 10);
}

static void bridge_apply_ysf_dch_slot(uint8_t *payload, uint8_t fn, ysf2dmr_bridge_t *b)
{
    uint8_t dch[10];
    uint8_t rid[5];

    memset(dch, ' ', 10);
    radio_id_to_dch5(b->radio_id, rid);
    switch (fn) {
    case 0:
        memset(dch, '*', 5);
        memcpy(dch + 5, rid, 5);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 1:
        memcpy(dch, b->net_src, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 2:
        memcpy(dch, b->net_dst, 10);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 5:
        memcpy(dch + 5, rid, 5);
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    case 6:
    case 7:
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    default:
        ysf_payload_write_vd_mode2_dch(payload, dch);
        break;
    }
}

static void bridge_fill_ysfd_headers(uint8_t *frame, const peer_ysf_t *ysf,
                                     const char net_src[10])
{
    memcpy(frame, "YSFD", 4);
    memcpy(frame + 4, ysf->callsign, 10);
    memcpy(frame + 14, net_src, 10);
    memcpy(frame + 24, YSF_WIRE_DST_ALL, 10);
    frame[34] = 0;
}

static void bridge_send_ysfd(ysf2dmr_bridge_t *b, uint8_t fi, uint8_t ft, uint8_t cm,
                             uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                             const uint8_t csd1[20], const uint8_t csd2[20])
{
    uint8_t frame[155];

    bridge_fill_ysfd_headers(frame, &b->ysf, b->net_src);
    frame[34] = net_cnt;
    /* Wire layout: sync at +35, FICH at +40 (CYSFFICH::encode skips the sync
     * internally; our fich_encode does not, so pass frame+40 explicitly). */
    if (fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR) {
        memset(frame + YSF_FICH_OFFSET_NET, 0, 120);
        memcpy(frame + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5);
        ysf_fich_encode_outbound(frame + YSF_FICH_OFFSET_RX, b->ysf.dgid, fich_fn, fi, ft, cm);
        if (csd1 && csd2)
            ysf_payload_write_header(frame + YSF_FICH_OFFSET_NET, csd1, csd2);
    } else {
        memcpy(frame + YSF_FICH_OFFSET_NET, payload120, 120);
        memcpy(frame + YSF_FICH_OFFSET_NET, YSF_SYNC_BYTES, 5);
        ysf_fich_encode_outbound(frame + YSF_FICH_OFFSET_RX, b->ysf.dgid, fich_fn, fi, ft, cm);
    }
    peer_ysf_send_ysfd(&b->ysf, frame, 155);
    b->ysf_fn = (uint8_t)(b->ysf_fn + 2);
    stamp_now(&b->last_ysf_tx);
    {
        static int tx_log;
        uint8_t wire_fi, wire_fn, wire_ft, wire_cm, wire_dt;
        uint8_t scratch[155];

        if (dbg_periodic(&tx_log) || fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR) {
            memcpy(scratch, frame, sizeof(scratch));
            ysf_fich_rewrite_dgid(scratch, b->ysf.dgid);
            if (ysf_fich_decode_fields(scratch, &wire_fi, &wire_fn, &wire_ft, &wire_cm, &wire_dt) == 0)
                LOG_DEBUG("YSF TX %s fi=%u ft=%u fn=%u cm=%u dt=%u net=%u src=%.10s dst=%.10s dgid=%u\n",
                    ysf_fi_name(fi), (unsigned)wire_fi, (unsigned)ft, (unsigned)wire_fn,
                    (unsigned)wire_cm, (unsigned)wire_dt,
                    (unsigned)net_cnt, b->net_src, scratch + 24, (unsigned)b->ysf.dgid);
        }
    }
}

static void bridge_emit_ysf_from_conv(ysf2dmr_bridge_t *b)
{
    uint8_t payload[120];
    unsigned int tag;
    static int drain_log;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(payload);
    if (tag == MODECONV_TAG_NODATA)
        return;

    if (dbg_periodic(&drain_log))
        LOG_DEBUG("ModeConv->YSF %s (dmr_in=%d ysf_cnt=%d)\n",
            modeconv_tag_name(tag), b->dmr_voice_frames, b->ysf_cnt);

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        b->ysf_cnt = 0;
        bridge_fill_ysf_csd(b, csd1, csd2);
        bridge_send_ysfd(b, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        b->ysf_cnt = 1;
        return;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        bridge_fill_ysf_csd(b, csd1, csd2);
        bridge_send_ysfd(b, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                         b->ysf_cnt, NULL, csd1, csd2);
        if (b->dmr_dmrd_other)
            LOG_INFO("DMR->YSF call end (%d voice frames in, %d other DMRD)\n",
                     b->dmr_voice_frames, b->dmr_dmrd_other);
        else
            LOG_INFO("DMR->YSF call end (%d voice frames in)\n", b->dmr_voice_frames);
        bridge_reset_call(b);
        return;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((b->ysf_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((b->ysf_cnt & 0x7FU) << 1);

        bridge_apply_ysf_dch_slot(payload, fn, b);
        bridge_send_ysfd(b, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM,
                         fn, net, payload, NULL, NULL);
        b->ysf_cnt++;
    }
}

void bridge_on_dmrd(ysf2dmr_bridge_t *b, const uint8_t *pkt, int len)
{
    static int rx_log;
    int rf, dst;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0) {
        LOG_DEBUG("DMR RX ignore len=%d (expected DMRD 55)\n", len);
        return;
    }

    rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];
    if (dbg_periodic(&rx_log)
        || dmrd_is_header(pkt, len) || dmrd_is_terminator(pkt, len)) {
        uint8_t ft, dtype;
        dmrd_parse_b15(pkt[15], &ft, &dtype);
        LOG_DEBUG("DMR RX %s b15=0x%02x rf=%d dst=%d gw=%u seq=%u stream=0x%08x active=%d\n",
            dmrd_class_label(pkt, len), pkt[15], rf, dst,
            (unsigned)((pkt[11] << 24) | (pkt[12] << 16) | (pkt[13] << 8) | pkt[14]),
            (unsigned)pkt[4], (unsigned)*(const uint32_t *)(pkt + 16), b->call_active);
    }

    if (dmrd_is_header(pkt, len)) {
        if (!b->call_active) {
            b->call_active = 1;
            b->dmr_stream_id = *(const uint32_t *)(pkt + 16);
            if (b->dmr_stream_id == 0)
                b->dmr_stream_id = new_stream_id();
            b->dmr_seq = 0;
            b->dmr_voice_frames = 0;
            b->dmr_tx_frames = 0;
            b->dmr_dmrd_other = 0;
            b->ysf_fn = 0;
            b->ysf_cnt = 0;
            modeconv_reset();
            bridge_set_dmr_rx_identity(b, pkt);
            LOG_INFO("DMR->YSF call start (TG %d, src %.10s)\n", b->dmr.tg, b->net_src);
        } else {
            bridge_set_dmr_rx_identity(b, pkt);
        }
        modeconv_put_dmr_header();
        return;
    }
    if (dmrd_is_terminator(pkt, len)) {
        LOG_DEBUG("DMR RX VTERM -> ModeConv EOT\n");
        if (b->call_active)
            modeconv_put_dmr_eot();
        return;
    }
    if (dmrd_is_voice(pkt, len)) {
        if (!b->call_active) {
            b->call_active = 1;
            b->dmr_stream_id = *(const uint32_t *)(pkt + 16);
            if (b->dmr_stream_id == 0)
                b->dmr_stream_id = new_stream_id();
            b->dmr_seq = pkt[4];
            b->dmr_voice_frames = 0;
            b->dmr_tx_frames = 0;
            b->dmr_dmrd_other = 0;
            b->ysf_fn = 0;
            b->ysf_cnt = 0;
            modeconv_reset();
            bridge_set_dmr_rx_identity(b, pkt);
            modeconv_put_dmr_header();
            LOG_INFO("DMR->YSF late entry (src %.10s)\n", b->net_src);
        }
        modeconv_put_dmr_voice(pkt + 20);
        b->dmr_voice_frames++;
        return;
    }
    if (b->call_active) {
        uint8_t ft, dtype;
        dmrd_parse_b15(pkt[15], &ft, &dtype);
        b->dmr_dmrd_other++;
        if (b->dmr_dmrd_other <= 3)
            LOG_WARNING("DMR RX unclassified b15=0x%02x (ft=%u dtype=%u)\n",
                        pkt[15], (unsigned)ft, (unsigned)dtype);
    }
}

void bridge_on_ysfd(ysf2dmr_bridge_t *b, const uint8_t *pkt, int len)
{
    uint8_t fi, fn, ft, cm, dt;
    char rpt[11], src[11];
    uint8_t rx_dgid;

    if (len != 155 || memcmp(pkt, "YSFD", 4) != 0) {
        LOG_DEBUG("YSF RX ignore len=%d tag=%.4s\n", len, pkt);
        return;
    }

    if (ysf_fich_decode_fields(pkt, &fi, &fn, &ft, &cm, &dt) != 0) {
        LOG_DEBUG("YSF RX FICH decode failed\n");
        return;
    }

    rx_dgid = ysf_fich_get_dgid();
    dbg_label10(rpt, pkt + 4);
    dbg_label10(src, pkt + 14);
    LOG_DEBUG("YSF RX %s fi=%u(%s) fn=%u ft=%u cm=%u dt=%u dgid=%u rpt=%s src=%s fn48=%u\n",
        ysf_fi_name(fi), (unsigned)fi, ysf_fi_name(fi), (unsigned)fn, (unsigned)ft,
        (unsigned)cm, (unsigned)dt, (unsigned)rx_dgid, rpt, src, (unsigned)pkt[48]);

    /* DG-ID 0 is untagged/open traffic: the reflector only relays what our
     * activated room should hear, so accept it; skip only other rooms (>=1). */
    if (b->ysf.dgid >= 1U && rx_dgid >= 1U && rx_dgid != b->ysf.dgid) {
        LOG_DEBUG("YSF RX skip DGID %u (want %u)\n",
            (unsigned)rx_dgid, (unsigned)b->ysf.dgid);
        return;
    }

    if (fi == YSF_FI_HEADER) {
        if (!bridge_resolve_ysf_header(b, pkt) || !bridge_ysf_talker_ready(b)) {
            LOG_WARNING("YSF ignored HEADER: no talker DMR id\n");
            return;
        }
        LOG_DEBUG("YSF process HEADER -> ModeConv (talker id %d)\n", b->ysf_rf_id);
        if (!b->call_active) {
            b->call_active = 1;
            b->dmr_stream_id = new_stream_id();
            b->dmr_seq = 0;
            b->dmr_tx_frames = 0;
            b->dmr_voice_frames = 0;
            b->ysf_fn = pkt[48];
            modeconv_reset();
            LOG_INFO("YSF->DMR call start (TG %d, talker %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->ysf_rf_id);
        }
        modeconv_put_ysf_header();
        return;
    }

    if (fi == YSF_FI_TERMINATOR) {
        LOG_DEBUG("YSF process EOT -> ModeConv (voice_in=%d)\n", b->ysf_voice_frames);
        if (b->call_active) {
            modeconv_put_ysf_eot();
            LOG_INFO("YSF->DMR terminator (%d voice frames in)\n", b->ysf_voice_frames);
        }
        return;
    }

    if (fi == YSF_FI_COMMUNICATIONS) {
        if (!bridge_ysf_talker_ready(b)) {
            LOG_WARNING("YSF ignored VOICE: no talker id (missing HEADER?)\n");
            return;
        }
        if (dt != YSF_DT_VD_MODE2) {
            LOG_DEBUG("YSF VOICE dt=%u (HP3ICC; YSF2DMR expects dt=2, using repack+putYSF)\n",
                      (unsigned)dt);
        }
        LOG_DEBUG("YSF process VOICE fn=%u -> ModeConv (talker id %d voice_in=%d)\n",
            (unsigned)fn, b->ysf_rf_id, b->ysf_voice_frames);
        if (!b->call_active) {
            b->call_active = 1;
            b->dmr_stream_id = new_stream_id();
            b->dmr_seq = 0;
            b->dmr_tx_frames = 0;
            b->dmr_voice_frames = 0;
            b->ysf_voice_frames = 0;
            b->ysf_fn = pkt[48];
            modeconv_reset();
            LOG_INFO("YSF->DMR call start (TG %d, talker %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->ysf_rf_id);
        }
        {
            uint8_t scratch[120];

            modeconv_put_ysf_payload(ysf_modeconv_chunk(pkt, scratch));
        }
        b->ysf_voice_frames++;
        return;
    }

    LOG_WARNING("YSF unhandled fi=%u ft=%u cm=%u dt=%u\n",
                (unsigned)fi, (unsigned)ft, (unsigned)cm, (unsigned)dt);
}

void bridge_tick(ysf2dmr_bridge_t *b)
{
    static time_t last_stall;

    if (!peer_dmr_connected(&b->dmr) || !peer_ysf_linked(&b->ysf)) {
        time_t now = time(NULL);
        if (log_level_enabled(LOG_LEVEL_DEBUG) && (now - last_stall >= 15 || last_stall == 0)) {
            LOG_DEBUG("tick idle: dmr=%s ysf=%s call_active=%d\n",
                peer_dmr_connected(&b->dmr) ? "up" : "down",
                peer_ysf_linked(&b->ysf) ? "up" : "down",
                b->call_active);
            last_stall = now;
        }
        return;
    }

    last_stall = 0;

    if (ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        bridge_emit_dmr_from_conv(b);

    if (ms_elapsed(&b->last_ysf_tx, YSF_FRAME_MS))
        bridge_emit_ysf_from_conv(b);
}

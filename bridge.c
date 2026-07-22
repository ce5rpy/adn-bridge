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
#include "media/bridge_util.h"
#include "media/call_meta.h"
#include "media/identity.h"
#include "mmdvm/modeconv_wrap.h"
#include "session/dmr_tx.h"
#include "session/dmr_wire.h"
#include "session/ysf_tx.h"
#include "talker_alias.h"
#include "ysf_fich.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BRIDGE_CALL_META(b) ((bridge_call_meta_t *)&(b)->net_src)

#define DMR_FRAME_MS  55
#define YSF_FRAME_MS  90
#define CONNECT_PTT_MS 500
#define DMR_CLEAR_DYNAMIC_TG 4000

static int bridge_tx_tg(const adn_bridge_t *b)
{
    if (b->connect_ptt_active && b->connect_ptt_tg > 0)
        return b->connect_ptt_tg;
    return b->dmr.tg;
}
#define YSF_DT_VD_MODE1 0x00U
#define YSF_DT_VOICE_FR 0x03U

static const char *modeconv_tag_name(unsigned int tag)
{
    switch (tag) {
    case MODECONV_TAG_HEADER: return "HEADER";
    case MODECONV_TAG_DATA:   return "DATA";
    case MODECONV_TAG_EOT:    return "EOT";
    default:                  return "NODATA";
    }
}

static uint8_t dmr_slot_bit_from_options(const char *options)
{
    (void)options;
    return 0x80; /* TX always TS2 */
}

static void bridge_send_dmrd(adn_bridge_t *b, uint8_t frame_type, const uint8_t *voice33);

static int bridge_ysf_talker_ready(const adn_bridge_t *b)
{
    return b->ysf_rf_id > 0 || b->dmr.dmrid > 0;
}

static int bridge_resolve_ysf_header(adn_bridge_t *b, const uint8_t *pkt)
{
    identity_ysf_ctx_t ctx = {
        b->aliases,
        b->dmr.dmrid,
        b->dmr.callsign,
    };

    return identity_resolve_ysf_header(BRIDGE_CALL_META(b), &b->ysf_rf_id, &ctx, pkt);
}

static void bridge_set_dmr_rx_identity(adn_bridge_t *b, const uint8_t *pkt)
{
    identity_dmr_ctx_t ctx = {
        b->aliases,
        b->dmr.dmrid,
        b->dmr.callsign,
        b->dmra.text,
        b->dmra.rf,
    };
    int rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    int dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];

    identity_resolve_dmr_to_ysf(BRIDGE_CALL_META(b), &ctx, rf, dst);
}

void bridge_on_dmra(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    int rf, block_id;
    uint8_t payload7[7];
    char decoded[32];

    if (!dmra_parse_packet(pkt, len, &rf, &block_id, payload7))
        return;

    if (b->dmra.rf != 0 && b->dmra.rf != rf) {
        memset(&b->dmra, 0, sizeof(b->dmra));
    }
    b->dmra.rf = rf;
    memcpy(b->dmra.blocks[block_id], payload7, 7);
    b->dmra.have |= (1U << (unsigned)block_id);

    if (dmra_decode_blocks(b->dmra.blocks, b->dmra.have, decoded, sizeof(decoded))) {
        strncpy(b->dmra.text, decoded, sizeof(b->dmra.text) - 1);
        b->dmra.text[sizeof(b->dmra.text) - 1] = '\0';
        LOG_DMR_DEBUG("DMR DMRA rf=%d block=%d text='%s'\n",
                  rf, block_id, b->dmra.text);
    }
}

static void bridge_reset_call(adn_bridge_t *b)
{
    b->call_active = 0;
    b->dmr_voice_frames = 0;
    b->dmr_tx_frames = 0;
    b->dmr_dmrd_other = 0;
    b->ysf_voice_frames = 0;
    b->ysf_rf_id = 0;
    b->ysf_cnt = 0;
    b->dmr_last_dtype = 0;
    memset(&b->dmra, 0, sizeof(b->dmra));
    memset(b->net_src, ' ', 10);
    memset(b->net_dst, ' ', 10);
    modeconv_reset();
}

void bridge_init(adn_bridge_t *b, const char *dmr_options,
                 adn_bridge_aliases_t *aliases, int default_ysf_dmrid,
                 int clear_dynamic_tg)
{
    memset(b, 0, sizeof(*b));
    b->aliases = aliases;
    b->default_ysf_dmrid = default_ysf_dmrid;
    b->clear_dynamic_tg = clear_dynamic_tg ? 1 : 0;
    b->dmr_slot_bit = dmr_slot_bit_from_options(dmr_options);
    memset(b->net_src, ' ', 10);
    memset(b->net_dst, ' ', 10);
    memcpy(b->net_dst, "ALL       ", 10);
    modeconv_init();
    bridge_stamp_now(&b->last_dmr_tx);
    bridge_stamp_now(&b->last_ysf_tx);
}

static void bridge_connect_ptt_begin_stream(adn_bridge_t *b, int tg, int clearing)
{
    b->connect_ptt_active = 1;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
    b->connect_ptt_tg = tg;
    b->connect_ptt_clearing = clearing ? 1 : 0;
    b->dmr_stream_id = bridge_new_stream_id();
    b->dmr_seq = 0;
    bridge_stamp_now(&b->connect_ptt_start);
    bridge_stamp_now(&b->last_dmr_tx);
    b->last_dmr_tx.tv_sec = 0; /* force first emit immediately */
    LOG_DMR_INFO("DMR connect PTT start (TG %d, %d ms)%s\n",
                 tg, CONNECT_PTT_MS,
                 clearing ? " [clear dynamic]" : "");
}

static void bridge_connect_ptt_finish(adn_bridge_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;
    int ended_tg = b->connect_ptt_tg > 0 ? b->connect_ptt_tg : b->dmr.tg;
    int was_clearing = b->connect_ptt_clearing;

    /* Pad to end of 6-frame superframe, then VTERM (same as ModeConv EOT). */
    while ((b->connect_ptt_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        bridge_send_dmrd(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
    bridge_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                     DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames)\n",
                 ended_tg, b->connect_ptt_voice_frames);

    if (was_clearing && b->dmr.tg > 0) {
        bridge_connect_ptt_begin_stream(b, b->dmr.tg, 0);
        return;
    }

    b->connect_ptt_active = 0;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
    b->connect_ptt_tg = 0;
    b->connect_ptt_clearing = 0;
}

void bridge_abort_connect_ptt(adn_bridge_t *b)
{
    if (!b->connect_ptt_active)
        return;
    if (b->connect_ptt_phase > 0) {
        /* Abort mid-stream: finish current TG only (skip follow-on activate). */
        b->connect_ptt_clearing = 0;
        bridge_connect_ptt_finish(b);
    } else {
        b->connect_ptt_active = 0;
        b->connect_ptt_phase = 0;
        b->connect_ptt_voice_frames = 0;
        b->connect_ptt_tg = 0;
        b->connect_ptt_clearing = 0;
    }
}

static void bridge_start_connect_ptt(adn_bridge_t *b)
{
    if (b->call_active || b->connect_ptt_active)
        return;
    if (b->dmr.tg <= 0)
        return;

    if (b->clear_dynamic_tg)
        bridge_connect_ptt_begin_stream(b, DMR_CLEAR_DYNAMIC_TG, 1);
    else
        bridge_connect_ptt_begin_stream(b, b->dmr.tg, 0);
}

static void bridge_emit_connect_ptt(adn_bridge_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;
    int i;

    if (!b->connect_ptt_active)
        return;

    if (b->connect_ptt_phase == 0) {
        for (i = 0; i < 3; i++)
            bridge_send_dmrd(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                             NULL);
        b->connect_ptt_phase = 1;
        bridge_stamp_now(&b->connect_ptt_start);
        return;
    }

    if (bridge_ms_since(&b->connect_ptt_start) >= CONNECT_PTT_MS) {
        bridge_connect_ptt_finish(b);
        return;
    }

    {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        bridge_send_dmrd(b, b15, DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
}

static void bridge_poll_connect_ptt(adn_bridge_t *b)
{
    int connected = peer_dmr_connected(&b->dmr);

    if (connected && !b->dmr_was_connected)
        bridge_start_connect_ptt(b);
    if (!connected) {
        b->connect_ptt_active = 0;
        b->connect_ptt_phase = 0;
        b->connect_ptt_voice_frames = 0;
        b->connect_ptt_tg = 0;
        b->connect_ptt_clearing = 0;
    }
    b->dmr_was_connected = connected;

    if (b->connect_ptt_active && bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        bridge_emit_connect_ptt(b);
}

static uint8_t dmrd_b15_dtype(const uint8_t *pkt)
{
    uint8_t ft, dtype;

    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return dtype;
}

static void bridge_begin_dmr_to_ysf(adn_bridge_t *b, const uint8_t *pkt)
{
    b->call_active = 1;
    b->dmr_stream_id = *(const uint32_t *)(pkt + 16);
    if (b->dmr_stream_id == 0)
        b->dmr_stream_id = bridge_new_stream_id();
    b->dmr_seq = 0;
    b->dmr_voice_frames = 0;
    b->dmr_tx_frames = 0;
    b->dmr_dmrd_other = 0;
    b->ysf_fn = 0;
    b->ysf_cnt = 0;
    modeconv_reset();
    bridge_set_dmr_rx_identity(b, pkt);
}

static void bridge_send_dmrd(adn_bridge_t *b, uint8_t frame_type, const uint8_t *voice33)
{
    dmr_tx_args_t args;
    int rf_id = (b->ysf_rf_id > 0) ? b->ysf_rf_id : b->dmr.dmrid;

    args.peer = &b->dmr;
    args.bridge_dmrid = b->dmr.dmrid;
    args.talker_rf_id = rf_id;
    args.tx_tg = bridge_tx_tg(b);
    args.seq = &b->dmr_seq;
    args.stream_id = b->dmr_stream_id;
    args.last_tx = &b->last_dmr_tx;
    dmr_tx_send(&args, frame_type, voice33);
}

static void bridge_emit_dmr_from_conv(adn_bridge_t *b)
{
    uint8_t voice[33];
    unsigned int tag = modeconv_get_dmr(voice);
    uint8_t slot_bit = b->dmr_slot_bit;
    static int drain_log;

    if (tag == MODECONV_TAG_NODATA)
        return;

    if (bridge_dbg_periodic(&drain_log))
        LOG_DMR_DEBUG("ModeConv->DMR %s (tx_frames=%d ysf_in=%d)\n",
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
        LOG_DMR_INFO("YSF->DMR call end (%d DMR frames out, talker %.10s)\n",
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

static int bridge_send_ysfd(adn_bridge_t *b, uint8_t fi, uint8_t ft, uint8_t cm,
                             uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                             const uint8_t csd1[20], const uint8_t csd2[20])
{
    ysf_tx_args_t args = {
        .peer = &b->ysf,
        .repeater_callsign = b->ysf.callsign,
        .meta = BRIDGE_CALL_META(b),
        .last_tx = &b->last_ysf_tx,
        .ysf_fn = &b->ysf_fn,
        .dgid_cfg = b->ysf.dgid,
    };

    return ysf_tx_send(&args, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2);
}

static int bridge_emit_ysf_from_conv(adn_bridge_t *b)
{
    uint8_t payload[120];
    unsigned int tag;
    static int drain_log;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(payload);
    if (tag == MODECONV_TAG_NODATA)
        return 0;

    if (bridge_dbg_periodic(&drain_log))
        LOG_YSF_DEBUG("ModeConv->YSF %s (dmr_in=%d ysf_cnt=%d)\n",
            modeconv_tag_name(tag), b->dmr_voice_frames, b->ysf_cnt);

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        b->ysf_cnt = 0;
        ysf_tx_fill_csd(BRIDGE_CALL_META(b), csd1, csd2);
        bridge_send_ysfd(b, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        b->ysf_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        ysf_tx_fill_csd(BRIDGE_CALL_META(b), csd1, csd2);
        bridge_send_ysfd(b, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                         b->ysf_cnt, NULL, csd1, csd2);
        if (b->dmr_dmrd_other)
            LOG_DMR_INFO("DMR->YSF call end (%d voice frames in, %d other DMRD)\n",
                     b->dmr_voice_frames, b->dmr_dmrd_other);
        else
            LOG_DMR_INFO("DMR->YSF call end (%d voice frames in)\n", b->dmr_voice_frames);
        bridge_reset_call(b);
        return 1;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((b->ysf_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((b->ysf_cnt & 0x7FU) << 1);

        bridge_send_ysfd(b, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM,
                         fn, net, payload, NULL, NULL);
        b->ysf_cnt++;
        return 1;
    }
    return 0;
}

void bridge_on_dmrd(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    static int rx_log;
    int rf, dst;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0) {
        LOG_DMR_DEBUG("DMR RX ignore len=%d (expected DMRD 55)\n", len);
        return;
    }

    rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];
    if (bridge_dbg_periodic(&rx_log)
        || dmrd_is_header(pkt, len) || dmrd_is_terminator(pkt, len)) {
        uint8_t ft, dtype;
        dmrd_parse_b15(pkt[15], &ft, &dtype);
        LOG_DMR_DEBUG("DMR RX %s b15=0x%02x rf=%d dst=%d gw=%u seq=%u stream=0x%08x active=%d\n",
            dmrd_class_label(pkt, len), pkt[15], rf, dst,
            (unsigned)((pkt[11] << 24) | (pkt[12] << 16) | (pkt[13] << 8) | pkt[14]),
            (unsigned)pkt[4], (unsigned)*(const uint32_t *)(pkt + 16), b->call_active);
    }

    if (dmrd_is_header(pkt, len)) {
        uint32_t stream = *(const uint32_t *)(pkt + 16);
        uint8_t dtype = DMRD_DTYPE_VHEAD;

        if (!b->call_active
            || (stream != 0 && stream != b->dmr_stream_id)) {
            bridge_begin_dmr_to_ysf(b, pkt);
            LOG_DMR_INFO("DMR->YSF call start (TG %d, src %.10s)\n", b->dmr.tg, b->net_src);
        } else {
            bridge_set_dmr_rx_identity(b, pkt);
        }
        /* YSF2DMR: putDMRHeader only on transition to VHEAD (m_dmrLastDT gate). */
        if (dtype != b->dmr_last_dtype)
            modeconv_put_dmr_header();
        else
            LOG_DMR_DEBUG("DMR->YSF ignore duplicate VHEAD (src %.10s)\n", b->net_src);
        b->dmr_last_dtype = dtype;
        return;
    }
    if (dmrd_is_terminator(pkt, len)) {
        LOG_DMR_DEBUG("DMR RX VTERM -> ModeConv EOT\n");
        if (b->call_active)
            modeconv_put_dmr_eot();
        b->dmr_last_dtype = DMRD_DTYPE_VTERM;
        return;
    }
    if (dmrd_is_voice(pkt, len)) {
        uint8_t dtype = dmrd_b15_dtype(pkt);

        if (!b->call_active) {
            bridge_begin_dmr_to_ysf(b, pkt);
            b->dmr_seq = pkt[4];
            modeconv_put_dmr_header();
            LOG_DMR_INFO("DMR->YSF late entry (src %.10s)\n", b->net_src);
        }
        modeconv_put_dmr_voice(pkt + 20);
        b->dmr_voice_frames++;
        b->dmr_last_dtype = dtype;
        return;
    }
    if (b->call_active) {
        uint8_t ft, dtype;
        dmrd_parse_b15(pkt[15], &ft, &dtype);
        b->dmr_dmrd_other++;
        if (b->dmr_dmrd_other <= 3)
            LOG_DMR_WARNING("DMR RX unclassified b15=0x%02x (ft=%u dtype=%u)\n",
                        pkt[15], (unsigned)ft, (unsigned)dtype);
    }
}

void bridge_on_ysfd(adn_bridge_t *b, const uint8_t *pkt, int len)
{
    uint8_t fi, fn, ft, cm, dt;
    char rpt[11], src[11];
    uint8_t rx_dgid;

    if (len != 155 || memcmp(pkt, "YSFD", 4) != 0) {
        LOG_YSF_DEBUG("YSF RX ignore len=%d tag=%.4s\n", len, pkt);
        return;
    }

    if (ysf_fich_decode_fields(pkt, &fi, &fn, &ft, &cm, &dt) != 0) {
        LOG_YSF_DEBUG("YSF RX FICH decode failed\n");
        return;
    }

    rx_dgid = ysf_fich_get_dgid();
    identity_dbg_label10(rpt, pkt + 4);
    identity_dbg_label10(src, pkt + 14);
    LOG_YSF_DEBUG("YSF RX %s fi=%u(%s) fn=%u ft=%u cm=%u dt=%u dgid=%u rpt=%s src=%s fn48=%u\n",
        ysf_tx_fi_name(fi), (unsigned)fi, ysf_tx_fi_name(fi), (unsigned)fn, (unsigned)ft,
        (unsigned)cm, (unsigned)dt, (unsigned)rx_dgid, rpt, src, (unsigned)pkt[48]);

    /* DG-ID 0 is untagged/open traffic: the reflector only relays what our
     * activated room should hear, so accept it; skip only other rooms (>=1). */
    if (b->ysf.dgid >= 1U && rx_dgid >= 1U && rx_dgid != b->ysf.dgid) {
        LOG_YSF_DEBUG("YSF RX skip DGID %u (want %u)\n",
            (unsigned)rx_dgid, (unsigned)b->ysf.dgid);
        return;
    }

    if (fi == YSF_FI_HEADER) {
        if (!bridge_resolve_ysf_header(b, pkt) || !bridge_ysf_talker_ready(b)) {
            LOG_YSF_WARNING("YSF ignored HEADER: no talker DMR id\n");
            return;
        }
        LOG_YSF_DEBUG("YSF process HEADER -> ModeConv (talker id %d)\n", b->ysf_rf_id);
        if (!b->call_active) {
            bridge_abort_connect_ptt(b);
            b->call_active = 1;
            b->dmr_stream_id = bridge_new_stream_id();
            b->dmr_seq = 0;
            b->dmr_tx_frames = 0;
            b->dmr_voice_frames = 0;
            b->ysf_fn = pkt[48];
            modeconv_reset();
            LOG_DMR_INFO("YSF->DMR call start (TG %d, talker %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->ysf_rf_id);
        }
        modeconv_put_ysf_header();
        return;
    }

    if (fi == YSF_FI_TERMINATOR) {
        LOG_YSF_DEBUG("YSF process EOT -> ModeConv (voice_in=%d)\n", b->ysf_voice_frames);
        if (b->call_active) {
            modeconv_put_ysf_eot();
            LOG_DMR_INFO("YSF->DMR terminator (%d voice frames in)\n", b->ysf_voice_frames);
        }
        return;
    }

    if (fi == YSF_FI_COMMUNICATIONS) {
        if (!bridge_ysf_talker_ready(b)) {
            LOG_YSF_WARNING("YSF ignored VOICE: no talker id (missing HEADER?)\n");
            return;
        }
        if (dt != YSF_DT_VD_MODE2) {
            LOG_YSF_DEBUG("YSF VOICE dt=%u (HP3ICC; YSF2DMR expects dt=2, using repack+putYSF)\n",
                      (unsigned)dt);
        }
        LOG_YSF_DEBUG("YSF process VOICE fn=%u -> ModeConv (talker id %d voice_in=%d)\n",
            (unsigned)fn, b->ysf_rf_id, b->ysf_voice_frames);
        if (!b->call_active) {
            bridge_abort_connect_ptt(b);
            b->call_active = 1;
            b->dmr_stream_id = bridge_new_stream_id();
            b->dmr_seq = 0;
            b->dmr_tx_frames = 0;
            b->dmr_voice_frames = 0;
            b->ysf_voice_frames = 0;
            b->ysf_fn = pkt[48];
            modeconv_reset();
            LOG_DMR_INFO("YSF->DMR call start (TG %d, talker %.10s id %d)\n",
                     b->dmr.tg, b->net_src, b->ysf_rf_id);
        }
        {
            uint8_t scratch[120];

            modeconv_put_ysf_payload(ysf_tx_modeconv_chunk(pkt, scratch));
        }
        b->ysf_voice_frames++;
        return;
    }

    LOG_YSF_WARNING("YSF unhandled fi=%u ft=%u cm=%u dt=%u\n",
                (unsigned)fi, (unsigned)ft, (unsigned)cm, (unsigned)dt);
}

void bridge_tick(adn_bridge_t *b)
{
    static time_t last_stall;

    /* Connect PTT runs as soon as DMR is up (YSF link not required). */
    bridge_poll_connect_ptt(b);

    if (!peer_dmr_connected(&b->dmr) || !peer_ysf_linked(&b->ysf)) {
        time_t now = time(NULL);
        if (log_channel_enabled(LOG_CH_DMR, LOG_LEVEL_DEBUG) && (now - last_stall >= 15 || last_stall == 0)) {
            LOG_YSF_DEBUG("tick idle: dmr=%s ysf=%s call_active=%d ptt=%d\n",
                peer_dmr_connected(&b->dmr) ? "up" : "down",
                peer_ysf_linked(&b->ysf) ? "up" : "down",
                b->call_active, b->connect_ptt_active);
            last_stall = now;
        }
        return;
    }

    last_stall = 0;

    if (!b->connect_ptt_active && bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        bridge_emit_dmr_from_conv(b);

    if (bridge_ms_elapsed(&b->last_ysf_tx, YSF_FRAME_MS))
        (void)bridge_emit_ysf_from_conv(b);
}

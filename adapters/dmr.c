/*
 * YSF <-> DMR adapter.
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/dmr.h"
#include "adapters/ysf.h"
#include "hbp/dmr_codec.h"
#include "log.h"
#include "media/bridge_util.h"
#include "media/call_meta.h"
#include "media/identity.h"
#include "media/log_flow.h"
#include "media/peer_bus.h"
#include "media/router.h"
#include "mmdvm/modeconv_wrap.h"
#include "peer_dmr.h"
#include "session/dmr_tx.h"
#include "session/dmr_wire.h"
#include "talker_alias.h"
#include <string.h>
#include <time.h>

#define ADAPTER_META(b) ((bridge_call_meta_t *)&(b)->net_src)
#define DMR_FRAME_MS 55
#define CONNECT_PTT_MS 500
#define DMR_CLEAR_DYNAMIC_TG 4000

static int adapter_bridge_src_id(const adn_bridge_t *b, media_peer_kind_t kind)
{
    return b->router ? media_router_find_first(b->router, kind) : -1;
}

static int adapter_ingress_router_id(const adn_bridge_t *b, media_peer_kind_t kind)
{
    if (b->ingress_router_id >= 0 && b->router
        && b->ingress_router_id < b->router->n_peers
        && b->router->peers[b->ingress_router_id].kind == kind)
        return b->ingress_router_id;
    return adapter_bridge_src_id(b, kind);
}

static int adapter_dmr_router_take(adn_bridge_t *b, int peer_id)
{
    int active;

    if (!b->router || peer_id < 0)
        return 1;
    active = media_router_active_ingress(b->router);
    if (active >= 0 && active != peer_id)
        media_router_ingress_end(b->router, active);
    if (!media_router_ingress_allowed(b->router, peer_id))
        return 0;
    media_router_ingress_begin(b->router, peer_id);
    return 1;
}

static int adapter_dmr_tx_tg(const adn_bridge_t *b)
{
    if (b->connect_ptt_active && b->connect_ptt_tg > 0)
        return b->connect_ptt_tg;
    return b->dmr ? b->dmr->tg : 0;
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

uint8_t adapter_dmr_slot_bit_from_options(const char *options)
{
    (void)options;
    return 0x80; /* TX always TS2 */
}

static void adapter_dmrd_tx_one(adn_bridge_t *b, peer_dmr_t *dmr,
                                uint8_t frame_type, const uint8_t *voice33)
{
    dmr_tx_args_t args;
    int rf_id;

    if (!dmr)
        return;
    rf_id = (b->ysf_rf_id > 0) ? b->ysf_rf_id : dmr->dmrid;

    args.peer = dmr;
    args.bridge_dmrid = dmr->dmrid;
    args.talker_rf_id = rf_id;
    args.tx_tg = adapter_dmr_tx_tg(b);
    args.seq = &b->dmr_seq;
    args.stream_id = b->dmr_stream_id;
    args.last_tx = &b->last_dmr_tx;
    dmr_tx_send(&args, frame_type, voice33);
}

typedef struct {
    adn_bridge_t    *b;
    uint8_t          frame_type;
    const uint8_t   *voice33;
} adapter_fanout_dmrd_ctx_t;

static int adapter_fanout_dmrd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    adapter_fanout_dmrd_ctx_t *ctx = vctx;
    peer_dmr_t *dmr;

    if (kind != MEDIA_PEER_DMR)
        return 0;
    dmr = media_peer_bus_dmr(ctx->b->bus, dst_id);
    if (!dmr)
        return 0;
    adapter_dmrd_tx_one(ctx->b, dmr, ctx->frame_type, ctx->voice33);
    return 0;
}

static void adapter_dmr_send_ysf(adn_bridge_t *b, uint8_t frame_type, const uint8_t *voice33)
{
    adapter_fanout_dmrd_ctx_t ctx = { b, frame_type, voice33 };
    int src = adapter_bridge_src_id(b, MEDIA_PEER_YSF);

    if (b->router && src >= 0)
        media_router_fanout(b->router, src, adapter_fanout_dmrd_cb, &ctx);
    else if (b->dmr)
        adapter_dmrd_tx_one(b, b->dmr, frame_type, voice33);
}

static void adapter_dmr_set_rx_identity(adn_bridge_t *b, const uint8_t *pkt)
{
    identity_dmr_ctx_t ctx;

    if (!b->dmr)
        return;
    ctx = (identity_dmr_ctx_t){
        b->aliases,
        b->dmr->dmrid,
        b->dmr->callsign,
        b->dmra.text,
        b->dmra.rf,
    };
    int rf = (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    int dst = (pkt[8] << 16) | (pkt[9] << 8) | pkt[10];

    identity_resolve_dmr_to_ysf(ADAPTER_META(b), &ctx, rf, dst);
}

void adapter_dmr_on_dmra_ysf(adn_bridge_t *b, const uint8_t *pkt, int len)
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

static void adapter_dmr_connect_ptt_begin_stream(adn_bridge_t *b, int tg, int clearing)
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

static void adapter_dmr_connect_ptt_finish(adn_bridge_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;
    int ended_tg = b->connect_ptt_tg > 0 ? b->connect_ptt_tg : (b->dmr ? b->dmr->tg : 0);
    int was_clearing = b->connect_ptt_clearing;

    /* Pad to end of 6-frame superframe, then VTERM (same as ModeConv EOT). */
    while ((b->connect_ptt_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        adapter_dmr_send_ysf(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
    adapter_dmr_send_ysf(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                     DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames)\n",
                 ended_tg, b->connect_ptt_voice_frames);

    if (was_clearing && b->dmr && b->dmr->tg > 0) {
        adapter_dmr_connect_ptt_begin_stream(b, b->dmr->tg, 0);
        return;
    }

    b->connect_ptt_active = 0;
    b->connect_ptt_phase = 0;
    b->connect_ptt_voice_frames = 0;
    b->connect_ptt_tg = 0;
    b->connect_ptt_clearing = 0;
}

void adapter_dmr_abort_connect_ptt_ysf(adn_bridge_t *b)
{
    if (!b->connect_ptt_active)
        return;
    if (b->connect_ptt_phase > 0) {
        /* Abort mid-stream: finish current TG only (skip follow-on activate). */
        b->connect_ptt_clearing = 0;
        adapter_dmr_connect_ptt_finish(b);
    } else {
        b->connect_ptt_active = 0;
        b->connect_ptt_phase = 0;
        b->connect_ptt_voice_frames = 0;
        b->connect_ptt_tg = 0;
        b->connect_ptt_clearing = 0;
    }
}

static void adapter_dmr_start_connect_ptt(adn_bridge_t *b)
{
    if (b->call_active || b->connect_ptt_active)
        return;
    if (!b->dmr || b->dmr->tg <= 0)
        return;

    if (b->clear_dynamic_tg)
        adapter_dmr_connect_ptt_begin_stream(b, DMR_CLEAR_DYNAMIC_TG, 1);
    else
        adapter_dmr_connect_ptt_begin_stream(b, b->dmr->tg, 0);
}

static void adapter_dmr_emit_connect_ptt(adn_bridge_t *b)
{
    uint8_t slot_bit = b->dmr_slot_bit;
    int i;

    if (!b->connect_ptt_active)
        return;

    if (b->connect_ptt_phase == 0) {
        for (i = 0; i < 3; i++)
            adapter_dmr_send_ysf(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                             NULL);
        b->connect_ptt_phase = 1;
        bridge_stamp_now(&b->connect_ptt_start);
        return;
    }

    if (bridge_ms_since(&b->connect_ptt_start) >= CONNECT_PTT_MS) {
        adapter_dmr_connect_ptt_finish(b);
        return;
    }

    {
        uint8_t n = (uint8_t)(b->connect_ptt_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        adapter_dmr_send_ysf(b, b15, DMR_SILENCE_DATA);
        b->connect_ptt_voice_frames++;
    }
}

void adapter_dmr_poll_connect_ptt_ysf(adn_bridge_t *b)
{
    int connected = b->dmr && peer_dmr_connected(b->dmr);

    if (connected && !b->dmr_was_connected)
        adapter_dmr_start_connect_ptt(b);
    if (!connected) {
        b->connect_ptt_active = 0;
        b->connect_ptt_phase = 0;
        b->connect_ptt_voice_frames = 0;
        b->connect_ptt_tg = 0;
        b->connect_ptt_clearing = 0;
    }
    b->dmr_was_connected = connected;

    if (b->connect_ptt_active && bridge_ms_elapsed(&b->last_dmr_tx, DMR_FRAME_MS))
        adapter_dmr_emit_connect_ptt(b);
}

static uint8_t adapter_dmrd_b15_dtype(const uint8_t *pkt)
{
    uint8_t ft, dtype;

    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return dtype;
}

static void adapter_dmr_begin_to_ysf(adn_bridge_t *b, const uint8_t *pkt)
{
    if (!adapter_dmr_router_take(b, adapter_ingress_router_id(b, MEDIA_PEER_DMR)))
        return;
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
    adapter_dmr_set_rx_identity(b, pkt);
}

void adapter_dmr_emit_from_conv_ysf(adn_bridge_t *b)
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
            adapter_dmr_send_ysf(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD), NULL);
        return;
    }
    if (tag == MODECONV_TAG_EOT) {
        /* YSF2DMR: pad with silence to the end of the 6-frame superframe. */
        while ((b->dmr_tx_frames % 6) != 0) {
            uint8_t n = (uint8_t)(b->dmr_tx_frames % 6);
            adapter_dmr_send_ysf(b, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
            b->dmr_tx_frames++;
        }
        adapter_dmr_send_ysf(b, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM), voice);
        LOG_DMR_INFO("%s call end (%d DMR frames out, talker %.10s)\n",
                media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR),
                b->dmr_tx_frames, b->net_src);
        adapter_ysf_dmr_reset_call(b);
        return;
    }
    if (tag == MODECONV_TAG_DATA) {
        /* n cycles 0..5 within the superframe; n=0 is the voice sync burst. */
        uint8_t n = (uint8_t)(b->dmr_tx_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        adapter_dmr_send_ysf(b, b15, voice);
        b->dmr_tx_frames++;
    }
}

void adapter_dmr_on_dmrd_ysf(adn_bridge_t *b, const uint8_t *pkt, int len)
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
            adapter_dmr_begin_to_ysf(b, pkt);
            LOG_DMR_INFO("%s call start (TG %d, src %.10s)\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF),
                         b->dmr ? b->dmr->tg : 0, b->net_src);
        } else {
            adapter_dmr_set_rx_identity(b, pkt);
        }
        /* YSF2DMR: putDMRHeader only on transition to VHEAD (m_dmrLastDT gate). */
        if (dtype != b->dmr_last_dtype)
            modeconv_put_dmr_header();
        else
            LOG_DMR_DEBUG("%s ignore duplicate VHEAD (src %.10s)\n",
                media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), b->net_src);
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
        uint8_t dtype = adapter_dmrd_b15_dtype(pkt);

        if (!b->call_active) {
            adapter_dmr_begin_to_ysf(b, pkt);
            b->dmr_seq = pkt[4];
            modeconv_put_dmr_header();
            LOG_DMR_INFO("%s late entry (src %.10s)\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), b->net_src);
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


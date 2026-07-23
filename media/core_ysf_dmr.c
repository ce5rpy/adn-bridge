/*
 * media_core — YSF<->DMR pathway (ported from bridge.c + adapters/dmr.c + adapters/ysf.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core_ysf_dmr.h"

#include "adapters/dmr.h"
#include "adapters/ysf.h"
#include "log.h"
#include "media/bridge_util.h"
#include "media/log_flow.h"
#include "mmdvm/modeconv_wrap.h"
#include "session/dmr_wire.h"
#include "talker_alias.h"

#include <string.h>

#define DMR_FRAME_MS   55
#define YSF_FRAME_MS   90
#define CONNECT_PTT_MS 500
#define DMR_CLEAR_DYNAMIC_TG 4000

/* Force-take: release whoever is active (if not us) then take. Mirrors
 * adapter_dmr_router_take/adapter_ysf_router_take — "last keyed wins"
 * half-duplex semantics, not a hard block (only 2 peers in this layout). */
static int core_router_take(media_core_t *core, int peer_id)
{
    int active;

    if (!core->router || peer_id < 0)
        return 1;
    active = media_router_active_ingress(core->router);
    if (active >= 0 && active != peer_id)
        media_router_ingress_end(core->router, active);
    if (!media_router_ingress_allowed(core->router, peer_id))
        return 0;
    media_router_ingress_begin(core->router, peer_id);
    return 1;
}

static void core_router_release_active(media_core_t *core)
{
    int active;

    if (!core->router)
        return;
    active = media_router_active_ingress(core->router);
    if (active >= 0)
        media_router_ingress_end(core->router, active);
}

static int core_dmr_tx_tg(const media_core_t *core, peer_dmr_t *dmr)
{
    if (core->connect_ptt_active && core->connect_ptt_tg > 0)
        return core->connect_ptt_tg;
    return dmr ? dmr->tg : 0;
}

/* Wire-correct multi-destination fan-out: each DMR destination connection
 * gets its own seq/stream, not one shared across all of them (Fase 5). Call
 * at every "new call toward DMR" point, mirroring core->dmr_seq/call.stream_id
 * reset for the single-shared-field bookkeeping kept for logging. */
static void core_reset_dmr_tx_slots(media_core_t *core)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (slot->kind != MEDIA_PEER_DMR)
            continue;
        slot->dmr_tx_seq = 0;
        slot->dmr_tx_stream_id = bridge_new_stream_id();
    }
}

/* ---- DMRA sidechain (talker alias assembly) ---- */

void core_ysf_dmr_merge_dmra(media_core_t *core, const media_bus_frame_t *frame)
{
    char decoded[32];
    int rf = frame->payload.dmra.rf;
    int block_id = frame->payload.dmra.block_id;

    if (core->dmra.rf != 0 && core->dmra.rf != rf)
        memset(&core->dmra, 0, sizeof(core->dmra));
    core->dmra.rf = rf;
    memcpy(core->dmra.blocks[block_id], frame->payload.dmra.block7, 7);
    core->dmra.have |= (1U << (unsigned)block_id);

    if (dmra_decode_blocks(core->dmra.blocks, core->dmra.have, decoded, sizeof(decoded))) {
        strncpy(core->dmra.text, decoded, sizeof(core->dmra.text) - 1);
        core->dmra.text[sizeof(core->dmra.text) - 1] = '\0';
        LOG_DMR_DEBUG("DMR DMRA rf=%d block=%d text='%s'\n", rf, block_id, core->dmra.text);
    }
}

/* ---- egress: DMR<-YSF (fan-out to all DMR peers) ---- */

typedef struct {
    media_core_t *core;
    uint8_t       frame_type;
    const uint8_t *voice33;
} core_fanout_dmrd_ctx_t;

static int core_fanout_dmrd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    core_fanout_dmrd_ctx_t *ctx = vctx;
    media_peer_slot_t *slot;
    peer_dmr_t *dmr;
    dmr_tx_args_t args;

    if (kind != MEDIA_PEER_DMR)
        return 0;
    slot = media_peer_bus_slot_mut(ctx->core->bus, dst_id);
    if (!slot || !slot->open)
        return 0;
    dmr = &slot->u.dmr;

    args.peer = dmr;
    args.bridge_dmrid = dmr->dmrid;
    args.talker_rf_id = (ctx->core->call.talker_id > 0) ? ctx->core->call.talker_id : dmr->dmrid;
    args.tx_tg = core_dmr_tx_tg(ctx->core, dmr);
    args.seq = &slot->dmr_tx_seq;
    args.stream_id = slot->dmr_tx_stream_id;
    args.last_tx = &ctx->core->last_dmr_tx;
    adapter_dmr_egress_dmrd(&args, ctx->frame_type, ctx->voice33);
    return 0;
}

static void core_send_dmrd(media_core_t *core, uint8_t frame_type, const uint8_t *voice33)
{
    core_fanout_dmrd_ctx_t ctx = { core, frame_type, voice33 };
    int src = media_router_find_first(core->router, MEDIA_PEER_YSF);

    if (core->router && src >= 0)
        media_router_fanout(core->router, src, core_fanout_dmrd_cb, &ctx);
}

/* ---- egress: YSF<-DMR (fan-out to all YSF peers) ---- */

typedef struct {
    media_core_t   *core;
    uint8_t         fi, ft, cm, fich_fn, net_cnt;
    const uint8_t  *payload120;
    const uint8_t  *csd1, *csd2;
} core_fanout_ysfd_ctx_t;

static int core_fanout_ysfd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    core_fanout_ysfd_ctx_t *ctx = vctx;
    peer_ysf_t *ysf;
    ysf_tx_args_t args;

    if (kind != MEDIA_PEER_YSF)
        return 0;
    ysf = media_peer_bus_ysf(ctx->core->bus, dst_id);
    if (!ysf)
        return 0;

    args = (ysf_tx_args_t){
        .peer = ysf,
        .repeater_callsign = ysf->callsign,
        .meta = &ctx->core->call.netcall,
        .last_tx = &ctx->core->last_ysf_tx,
        .ysf_fn = &ctx->core->ysf_fn,
        .dgid_cfg = ysf->dgid,
    };
    adapter_ysf_egress_ysfd(&args, ctx->fi, ctx->ft, ctx->cm, ctx->fich_fn, ctx->net_cnt,
                            ctx->payload120, ctx->csd1, ctx->csd2);
    return 0;
}

static int core_send_ysfd(media_core_t *core, uint8_t fi, uint8_t ft, uint8_t cm,
                          uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                          const uint8_t csd1[20], const uint8_t csd2[20])
{
    core_fanout_ysfd_ctx_t ctx = { core, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2 };
    int src = media_router_find_first(core->router, MEDIA_PEER_DMR);

    if (core->router && src >= 0)
        return media_router_fanout(core->router, src, core_fanout_ysfd_cb, &ctx) > 0;
    return 0;
}

/* ---- DMR ingress (peer -> YSF egress via ModeConv) ---- */

static void core_begin_dmr_to_ysf(media_core_t *core, int src_router_id,
                                  const media_bus_frame_t *frame)
{
    if (!core_router_take(core, src_router_id))
        return;
    core->phase = MEDIA_CALL_TX_TO_PEER;
    core->call.stream_id = frame->meta.stream_id != 0 ? frame->meta.stream_id
                                                       : bridge_new_stream_id();
    core->dmr_seq = frame->wire_seq;
    core->dmr_voice_frames = 0;
    core->dmr_tx_frames = 0;
    core->ysf_fn = 0;
    core->ysf_cnt = 0;
    modeconv_reset();
}

void core_ysf_dmr_ingress_dmr(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    core->call.netcall = frame->meta.netcall;
    core->call.talker_id = frame->meta.talker_id;

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN: {
        uint8_t dtype = DMRD_DTYPE_VHEAD;

        if (core->phase == MEDIA_CALL_IDLE
            || (frame->meta.stream_id != 0 && frame->meta.stream_id != core->call.stream_id)) {
            core_begin_dmr_to_ysf(core, src_router_id, frame);
            LOG_DMR_INFO("%s call start (src %.10s)\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF),
                         core->call.netcall.net_src);
        }
        /* YSF2DMR: putDMRHeader only on transition to VHEAD (m_dmrLastDT gate). */
        if (dtype != core->dmr_last_dtype)
            modeconv_put_dmr_header();
        else
            LOG_DMR_DEBUG("%s ignore duplicate VHEAD (src %.10s)\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), core->call.netcall.net_src);
        core->dmr_last_dtype = dtype;
        return;
    }
    case MEDIA_FRAME_CALL_END:
        if (core->phase != MEDIA_CALL_IDLE)
            modeconv_put_dmr_eot();
        core->dmr_last_dtype = DMRD_DTYPE_VTERM;
        return;
    case MEDIA_FRAME_VOICE:
        if (core->phase == MEDIA_CALL_IDLE) {
            core_begin_dmr_to_ysf(core, src_router_id, frame);
            modeconv_put_dmr_header();
            LOG_DMR_INFO("%s late entry (src %.10s)\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), core->call.netcall.net_src);
        }
        modeconv_put_dmr_voice(frame->payload.dmr_voice33);
        core->dmr_voice_frames++;
        core->dmr_last_dtype = frame->wire_dtype;
        return;
    default:
        return;
    }
}

/* Drain one ModeConv->DMR unit (called from tick, paced at DMR_FRAME_MS). */
static void core_emit_dmr_from_conv(media_core_t *core)
{
    uint8_t voice[33];
    unsigned int tag = modeconv_get_dmr(voice);
    uint8_t slot_bit = core->dmr_slot_bit;

    if (tag == MODECONV_TAG_NODATA)
        return;

    if (tag == MODECONV_TAG_HEADER) {
        int i;
        for (i = 0; i < 3; i++)
            core_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD), NULL);
        return;
    }
    if (tag == MODECONV_TAG_EOT) {
        while ((core->dmr_tx_frames % 6) != 0) {
            uint8_t n = (uint8_t)(core->dmr_tx_frames % 6);
            core_send_dmrd(core, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
            core->dmr_tx_frames++;
        }
        core_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM), voice);
        LOG_DMR_INFO("%s call end (%d DMR frames out, talker %.10s)\n",
                     media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR),
                     core->dmr_tx_frames, core->call.netcall.net_src);
        core_router_release_active(core);
        core->phase = MEDIA_CALL_IDLE;
        core->dmr_voice_frames = 0;
        core->dmr_tx_frames = 0;
        core->ysf_voice_frames = 0;
        core->call.talker_id = 0;
        core->ysf_cnt = 0;
        core->dmr_last_dtype = 0;
        memset(&core->dmra, 0, sizeof(core->dmra));
        bridge_call_meta_clear(&core->call.netcall);
        modeconv_reset();
        return;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t n = (uint8_t)(core->dmr_tx_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
        core_send_dmrd(core, b15, voice);
        core->dmr_tx_frames++;
    }
}

/* ---- YSF ingress (peer -> DMR egress via ModeConv) ---- */

static void core_begin_ysf_to_dmr(media_core_t *core, int src_router_id)
{
    if (!core_router_take(core, src_router_id))
        return;
    core->phase = MEDIA_CALL_TX_TO_PEER;
    core->call.stream_id = bridge_new_stream_id();
    core->dmr_seq = 0;
    core->dmr_tx_frames = 0;
    core->dmr_voice_frames = 0;
    core->ysf_voice_frames = 0;
    core_reset_dmr_tx_slots(core);
    modeconv_reset();
}

void core_ysf_dmr_ingress_ysf(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN:
        core->call.netcall = frame->meta.netcall;
        core->call.talker_id = frame->meta.talker_id;
        if (core->phase == MEDIA_CALL_IDLE) {
            core_ysf_dmr_abort_connect_ptt(core);
            core_begin_ysf_to_dmr(core, src_router_id);
            LOG_DMR_INFO("%s call start (talker %.10s id %d)\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR),
                         core->call.netcall.net_src, core->call.talker_id);
        }
        modeconv_put_ysf_header();
        return;
    case MEDIA_FRAME_CALL_END:
        if (core->phase != MEDIA_CALL_IDLE) {
            modeconv_put_ysf_eot();
            LOG_DMR_INFO("%s terminator (%d voice frames in)\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR), core->ysf_voice_frames);
        }
        return;
    case MEDIA_FRAME_VOICE: {
        peer_dmr_t *dmr;

        if (core->call.talker_id <= 0) {
            dmr = media_peer_bus_primary_dmr(core->bus);
            if (!dmr || dmr->dmrid <= 0) {
                LOG_YSF_WARNING("YSF ignored VOICE: no talker id (missing HEADER?)\n");
                return;
            }
        }
        if (frame->wire_dtype != YSF_DT_VD_MODE2)
            LOG_YSF_DEBUG("YSF VOICE dt=%u (HP3ICC; YSF2DMR expects dt=2, using repack+putYSF)\n",
                          (unsigned)frame->wire_dtype);
        if (core->phase == MEDIA_CALL_IDLE) {
            core_ysf_dmr_abort_connect_ptt(core);
            core_begin_ysf_to_dmr(core, src_router_id);
            LOG_DMR_INFO("%s call start (talker %.10s id %d)\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR),
                         core->call.netcall.net_src, core->call.talker_id);
        }
        modeconv_put_ysf_payload(frame->payload.ysf_payload120);
        core->ysf_voice_frames++;
        return;
    }
    default:
        return;
    }
}

/* Drain one ModeConv->YSF unit (called from tick, paced at YSF_FRAME_MS). */
static int core_emit_ysf_from_conv(media_core_t *core)
{
    uint8_t payload[120];
    unsigned int tag;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(payload);
    if (tag == MODECONV_TAG_NODATA)
        return 0;

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        core->ysf_cnt = 0;
        ysf_tx_fill_csd(&core->call.netcall, csd1, csd2);
        core_send_ysfd(core, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        core->ysf_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        ysf_tx_fill_csd(&core->call.netcall, csd1, csd2);
        core_send_ysfd(core, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                       core->ysf_cnt, NULL, csd1, csd2);
        LOG_DMR_INFO("%s call end (%d voice frames in)\n",
                     media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), core->dmr_voice_frames);
        core_router_release_active(core);
        core->phase = MEDIA_CALL_IDLE;
        core->dmr_voice_frames = 0;
        core->dmr_tx_frames = 0;
        core->ysf_voice_frames = 0;
        core->call.talker_id = 0;
        core->ysf_cnt = 0;
        core->dmr_last_dtype = 0;
        memset(&core->dmra, 0, sizeof(core->dmra));
        bridge_call_meta_clear(&core->call.netcall);
        modeconv_reset();
        return 1;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((core->ysf_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((core->ysf_cnt & 0x7FU) << 1);

        core_send_ysfd(core, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM, fn, net,
                       payload, NULL, NULL);
        core->ysf_cnt++;
        return 1;
    }
    return 0;
}

/* ---- connect-PTT (DMR login rising edge: TG 4000 optional clear + activate) ---- */

static void core_connect_ptt_begin_stream(media_core_t *core, int tg, int clearing)
{
    core->connect_ptt_active = 1;
    core->connect_ptt_phase = 0;
    core->connect_ptt_voice_frames = 0;
    core->connect_ptt_tg = tg;
    core->connect_ptt_clearing = clearing ? 1 : 0;
    core->call.stream_id = bridge_new_stream_id();
    core->dmr_seq = 0;
    core_reset_dmr_tx_slots(core);
    bridge_stamp_now(&core->connect_ptt_start);
    bridge_stamp_now(&core->last_dmr_tx);
    core->last_dmr_tx.tv_sec = 0; /* force first emit immediately */
    LOG_DMR_INFO("DMR connect PTT start (TG %d, %d ms)%s\n",
                 tg, CONNECT_PTT_MS, clearing ? " [clear dynamic]" : "");
}

static void core_connect_ptt_finish(media_core_t *core, peer_dmr_t *dmr)
{
    uint8_t slot_bit = core->dmr_slot_bit;
    int ended_tg = core->connect_ptt_tg > 0 ? core->connect_ptt_tg : (dmr ? dmr->tg : 0);
    int was_clearing = core->connect_ptt_clearing;

    while ((core->connect_ptt_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(core->connect_ptt_voice_frames % 6);
        core_send_dmrd(core, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        core->connect_ptt_voice_frames++;
    }
    core_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                   DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames)\n",
                 ended_tg, core->connect_ptt_voice_frames);

    if (was_clearing && dmr && dmr->tg > 0) {
        core_connect_ptt_begin_stream(core, dmr->tg, 0);
        return;
    }
    core->connect_ptt_active = 0;
    core->connect_ptt_phase = 0;
    core->connect_ptt_voice_frames = 0;
    core->connect_ptt_tg = 0;
    core->connect_ptt_clearing = 0;
}

void core_ysf_dmr_abort_connect_ptt(media_core_t *core)
{
    if (!core->connect_ptt_active)
        return;
    if (core->connect_ptt_phase > 0) {
        core->connect_ptt_clearing = 0;
        core_connect_ptt_finish(core, media_peer_bus_primary_dmr(core->bus));
    } else {
        core->connect_ptt_active = 0;
        core->connect_ptt_phase = 0;
        core->connect_ptt_voice_frames = 0;
        core->connect_ptt_tg = 0;
        core->connect_ptt_clearing = 0;
    }
}

static void core_start_connect_ptt(media_core_t *core, peer_dmr_t *dmr)
{
    if (core->phase != MEDIA_CALL_IDLE || core->connect_ptt_active)
        return;
    if (!dmr || dmr->tg <= 0)
        return;
    if (core->clear_dynamic_tg)
        core_connect_ptt_begin_stream(core, DMR_CLEAR_DYNAMIC_TG, 1);
    else
        core_connect_ptt_begin_stream(core, dmr->tg, 0);
}

static void core_emit_connect_ptt(media_core_t *core)
{
    uint8_t slot_bit = core->dmr_slot_bit;
    int i;

    if (!core->connect_ptt_active)
        return;

    if (core->connect_ptt_phase == 0) {
        for (i = 0; i < 3; i++)
            core_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD), NULL);
        core->connect_ptt_phase = 1;
        bridge_stamp_now(&core->connect_ptt_start);
        return;
    }
    if (bridge_ms_since(&core->connect_ptt_start) >= CONNECT_PTT_MS) {
        core_connect_ptt_finish(core, media_peer_bus_primary_dmr(core->bus));
        return;
    }
    {
        uint8_t n = (uint8_t)(core->connect_ptt_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
        core_send_dmrd(core, b15, DMR_SILENCE_DATA);
        core->connect_ptt_voice_frames++;
    }
}

void core_ysf_dmr_poll_connect_ptt(media_core_t *core)
{
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
    peer_ysf_t *ysf = media_peer_bus_primary_ysf(core->bus);
    int connected;

    if (!dmr || !ysf)
        return; /* only meaningful when this layout's two peers exist */

    connected = peer_dmr_connected(dmr);
    if (connected && !core->dmr_was_connected)
        core_start_connect_ptt(core, dmr);
    if (!connected) {
        core->connect_ptt_active = 0;
        core->connect_ptt_phase = 0;
        core->connect_ptt_voice_frames = 0;
        core->connect_ptt_tg = 0;
        core->connect_ptt_clearing = 0;
    }
    core->dmr_was_connected = connected;

    if (core->connect_ptt_active && bridge_ms_elapsed(&core->last_dmr_tx, DMR_FRAME_MS))
        core_emit_connect_ptt(core);
}

/* ---- tick: pace ModeConv drain to the wire at DMR/YSF frame intervals ---- */

void core_ysf_dmr_tick(media_core_t *core)
{
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
    peer_ysf_t *ysf = media_peer_bus_primary_ysf(core->bus);

    if (!dmr || !ysf || !peer_dmr_connected(dmr) || !peer_ysf_linked(ysf))
        return;

    if (!core->connect_ptt_active && bridge_ms_elapsed(&core->last_dmr_tx, DMR_FRAME_MS))
        core_emit_dmr_from_conv(core);

    if (bridge_ms_elapsed(&core->last_ysf_tx, YSF_FRAME_MS))
        (void)core_emit_ysf_from_conv(core);
}

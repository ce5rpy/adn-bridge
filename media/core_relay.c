/*
 * media_core — same-protocol relay (DMR<->DMR, YSF<->YSF, EchoLink<->EchoLink).
 *
 * Wire-only fan-out: no ModeConv, no vocoder, same codec both ends. "Who's
 * active" is tracked via the router's shared active_ingress rather than a
 * core->phase/core->call session (unlike media/core_ysf_dmr.c and
 * media/core_echolink.c), so a relay call composes safely alongside a
 * concurrent DMR<->YSF or EchoLink<->* call in a 3+-kind bus — they touch
 * disjoint state (per-slot TX framing here, core-level ModeConv/vocoder
 * scalars there).
 *
 * The router-take/connect-PTT-adjacent helpers below intentionally shadow
 * media/core_ysf_dmr.c and media/core_echolink.c's own copies rather than
 * share them — those two modules already duplicate this logic on purpose
 * (see core_echolink.c's connect-PTT comment); adding a third small copy
 * here keeps the same precedent instead of coupling three pathway modules
 * through a new shared one.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core_relay.h"

#include "adapters/dmr.h"
#include "adapters/el.h"
#include "adapters/ysf.h"
#include "log.h"
#include "media/bridge_util.h"
#include "media/log_flow.h"
#include "session/dmr_wire.h"

#include <string.h>

#define EL_RELAY_HANG_MS 700

/* Check-then-take only, no force-release of a different active peer — same
 * (non-preempting) semantics as core_echolink.c's core_router_take, chosen so
 * relay never has to know how to abort a different pathway module's session
 * (see file header). A relay call simply waits for the bus to go idle. */
static int core_relay_router_take(media_core_t *core, int peer_id)
{
    if (!core->router || peer_id < 0)
        return 1;
    if (!media_router_ingress_allowed(core->router, peer_id))
        return 0;
    media_router_ingress_begin(core->router, peer_id);
    return 1;
}

static void core_relay_router_release(media_core_t *core, int peer_id)
{
    if (core->router && media_router_active_ingress(core->router) == peer_id)
        media_router_ingress_end(core->router, peer_id);
}

static int core_relay_is_active(const media_core_t *core, int peer_id)
{
    return core->router && media_router_active_ingress(core->router) == peer_id;
}

/* =====================================================================
 * DMR <-> DMR
 * ===================================================================== */

static int core_relay_dmr_tx_tg(const media_peer_slot_t *slot, const peer_dmr_t *dmr)
{
    if (slot->cp_active && slot->cp_tg > 0)
        return slot->cp_tg;
    return dmr ? dmr->tg : 0;
}

static void core_relay_dmr_tx_one(media_core_t *core, media_peer_slot_t *slot,
                                  int talker_rf_id, uint8_t frame_type, const uint8_t *voice33)
{
    peer_dmr_t *dmr;
    dmr_tx_args_t args;

    if (!slot || !slot->open)
        return;
    dmr = &slot->u.dmr;
    args.peer = dmr;
    args.bridge_dmrid = dmr->dmrid;
    args.talker_rf_id = talker_rf_id > 0 ? talker_rf_id : dmr->dmrid;
    args.tx_tg = core_relay_dmr_tx_tg(slot, dmr);
    args.seq = &slot->dmr_tx_seq;
    args.stream_id = slot->dmr_tx_stream_id;
    args.last_tx = &core->relay_last_dmr_tx;
    adapter_dmr_egress_dmrd(&args, frame_type, voice33);
}

typedef struct {
    media_core_t  *core;
    int            talker_rf_id;
    uint8_t        frame_type;
    const uint8_t *voice33;
} core_relay_dmrd_ctx_t;

static int core_relay_dmrd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    core_relay_dmrd_ctx_t *ctx = vctx;
    media_peer_slot_t *slot;

    if (kind != MEDIA_PEER_DMR)
        return 0;
    slot = media_peer_bus_slot_mut(ctx->core->bus, dst_id);
    core_relay_dmr_tx_one(ctx->core, slot, ctx->talker_rf_id, ctx->frame_type, ctx->voice33);
    return 0;
}

static void core_relay_send_dmrd(media_core_t *core, int src_router_id, int talker_rf_id,
                                 uint8_t frame_type, const uint8_t *voice33)
{
    core_relay_dmrd_ctx_t ctx = { core, talker_rf_id, frame_type, voice33 };

    if (core->router)
        media_router_fanout(core->router, src_router_id, core_relay_dmrd_cb, &ctx);
}

static void core_relay_reset_dmr_tx_slots(media_core_t *core)
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

void core_relay_dmr_to_dmr(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN:
        if (!core_relay_router_take(core, src_router_id))
            return;
        core_relay_reset_dmr_tx_slots(core);
        LOG_DMR_INFO("dmr->dmr relay call start (src %d)\n", frame->meta.talker_id);
        core_relay_send_dmrd(core, src_router_id, frame->meta.talker_id,
                             (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD), NULL);
        return;
    case MEDIA_FRAME_CALL_END:
        if (!core_relay_is_active(core, src_router_id))
            return;
        core_relay_send_dmrd(core, src_router_id, frame->meta.talker_id,
                             (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM), NULL);
        LOG_DMR_INFO("dmr->dmr relay call end\n");
        core_relay_router_release(core, src_router_id);
        return;
    case MEDIA_FRAME_VOICE:
        if (!core_relay_is_active(core, src_router_id)) {
            if (!core_relay_router_take(core, src_router_id))
                return;
            core_relay_reset_dmr_tx_slots(core);
            LOG_DMR_INFO("dmr->dmr relay late entry (src %d)\n", frame->meta.talker_id);
        }
        {
            uint8_t b15 = (uint8_t)(slot_bit | (frame->wire_dmr_ft << 4) | frame->wire_dtype);

            core_relay_send_dmrd(core, src_router_id, frame->meta.talker_id, b15,
                                 frame->payload.dmr_voice33);
        }
        return;
    default:
        return;
    }
}

/* =====================================================================
 * YSF <-> YSF
 * ===================================================================== */

typedef struct {
    media_core_t       *core;
    bridge_call_meta_t  netcall;
    uint8_t             fi, ft, cm, fich_fn, net_cnt;
    const uint8_t       *payload120, *csd1, *csd2;
} core_relay_ysfd_ctx_t;

static int core_relay_ysfd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    core_relay_ysfd_ctx_t *ctx = vctx;
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
        .meta = &ctx->netcall,
        .last_tx = &ctx->core->relay_last_ysf_tx,
        .dgid_cfg = ysf->dgid,
    };
    adapter_ysf_egress_ysfd(&args, ctx->fi, ctx->ft, ctx->cm, ctx->fich_fn, ctx->net_cnt,
                            ctx->payload120, ctx->csd1, ctx->csd2);
    return 0;
}

static void core_relay_send_ysfd(media_core_t *core, int src_router_id,
                                 const bridge_call_meta_t *netcall,
                                 uint8_t fi, uint8_t ft, uint8_t cm, uint8_t fich_fn,
                                 uint8_t net_cnt, const uint8_t *payload120,
                                 const uint8_t csd1[20], const uint8_t csd2[20])
{
    core_relay_ysfd_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;
    ctx.netcall = *netcall;
    ctx.fi = fi;
    ctx.ft = ft;
    ctx.cm = cm;
    ctx.fich_fn = fich_fn;
    ctx.net_cnt = net_cnt;
    ctx.payload120 = payload120;
    ctx.csd1 = csd1;
    ctx.csd2 = csd2;
    if (core->router)
        media_router_fanout(core->router, src_router_id, core_relay_ysfd_cb, &ctx);
}

static media_peer_slot_t *core_relay_ysf_src_slot(media_core_t *core, int src_router_id)
{
    return media_peer_bus_slot_mut(core->bus, src_router_id);
}

void core_relay_ysf_to_ysf(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    media_peer_slot_t *src = core_relay_ysf_src_slot(core, src_router_id);

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN: {
        uint8_t csd1[20], csd2[20];

        if (!core_relay_router_take(core, src_router_id))
            return;
        if (src)
            src->ysf_relay_cnt = 0;
        LOG_YSF_INFO("ysf->ysf relay call start (src %.10s)\n", frame->meta.netcall.net_src);
        ysf_tx_fill_csd(&frame->meta.netcall, csd1, csd2);
        core_relay_send_ysfd(core, src_router_id, &frame->meta.netcall,
                             YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        return;
    }
    case MEDIA_FRAME_CALL_END: {
        uint8_t csd1[20], csd2[20];

        if (!core_relay_is_active(core, src_router_id))
            return;
        ysf_tx_fill_csd(&frame->meta.netcall, csd1, csd2);
        core_relay_send_ysfd(core, src_router_id, &frame->meta.netcall,
                             YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                             src ? src->ysf_relay_cnt : 0, NULL, csd1, csd2);
        LOG_YSF_INFO("ysf->ysf relay call end\n");
        core_relay_router_release(core, src_router_id);
        return;
    }
    case MEDIA_FRAME_VOICE:
        if (!core_relay_is_active(core, src_router_id)) {
            if (!core_relay_router_take(core, src_router_id))
                return;
            if (src)
                src->ysf_relay_cnt = 0;
            LOG_YSF_INFO("ysf->ysf relay late entry (src %.10s)\n", frame->meta.netcall.net_src);
        }
        {
            uint8_t cnt = src ? src->ysf_relay_cnt : 0;
            uint8_t fn = (uint8_t)((cnt) % (YSF_FICH_FT + 1U));
            uint8_t net_cnt = (uint8_t)((cnt & 0x7FU) << 1);

            core_relay_send_ysfd(core, src_router_id, &frame->meta.netcall,
                                 YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM, fn, net_cnt,
                                 frame->payload.ysf_payload120, NULL, NULL);
            if (src)
                src->ysf_relay_cnt++;
        }
        return;
    default:
        return;
    }
}

/* =====================================================================
 * EchoLink <-> EchoLink (polled PCM, no wire frame to classify)
 * ===================================================================== */

static void core_relay_el_fanout(media_core_t *core, int src_router_id,
                                 const int16_t *pcm, int n)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (slot->kind != MEDIA_PEER_ECHOLINK || !slot->open || slot->router_id == src_router_id)
            continue;
        adapter_el_egress_pcm(&slot->u.el, pcm, n);
    }
}

void core_relay_el_to_el(media_core_t *core)
{
    int i;

    if (!core->router || !core->bus)
        return;

    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];
        int16_t pcm[160];
        int n;

        if (slot->kind != MEDIA_PEER_ECHOLINK || !slot->open)
            continue;
        if (!core_relay_is_active(core, slot->router_id)
            && media_router_active_ingress(core->router) >= 0)
            continue; /* someone else has the bus */

        n = adapter_el_read_pcm(&slot->u.el, pcm, 160);
        if (n <= 0) {
            if (core_relay_is_active(core, slot->router_id)
                && bridge_ms_elapsed(&core->last_el_relay_speech, EL_RELAY_HANG_MS))
                core_relay_router_release(core, slot->router_id);
            continue;
        }

        if (!core_relay_is_active(core, slot->router_id)) {
            if (!core_relay_router_take(core, slot->router_id))
                continue;
            LOG_EL_INFO("echolink->echolink relay call start\n");
        }
        bridge_stamp_now(&core->last_el_relay_speech);
        core_relay_el_fanout(core, slot->router_id, pcm, n);
    }
}

/*
 * media_core — EchoLink<->DMR and EchoLink<->YSF pathways (ported from bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core_echolink.h"

#include "adapters/dmr.h"
#include "adapters/el.h"
#include "adapters/ysf.h"
#include "log.h"
#include "media/bridge_util.h"
#include "media/identity.h"
#include "media/log_flow.h"
#include "mmdvm/modeconv_wrap.h"
#include "session/dmr_wire.h"
#include "ysf_fich.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DMR_FRAME_MS   60 /* bridge_el paces DMR TX slightly slower than adapters/dmr.c's 55ms */
#define YSF_FRAME_MS   90
#define CONNECT_PTT_MS 500               /* on-air duration of each connect-PTT burst (4000 and real TG alike) */
#define CONNECT_PTT_START_DELAY_MS 2000  /* silent gap after DMR connects, before the first connect-PTT starts */
#define CONNECT_PTT_GAP_MS         4000  /* silent gap after the 4000 clear-PTT ends, before the real-TG PTT starts */
#define DMR_CLEAR_DYNAMIC_TG 4000
/* End EL->DMR/YSF after this much without inbound EL PCM (key-down silence
 * must still hold / activate the TG; hang follows PCM presence, not RMS). */
#define EL_HANG_MS 700
/* After EL->DMR/YSF end, ignore residual conference PCM (no phantom reopen). */
#define EL_TX_COOLDOWN_MS 800
/* DMR/YSF->EL without VTERM/EOT used to leave the leg open and block EL TX. */
#define DMR_RX_HANG_MS 1500
#define YSF_RX_HANG_MS 1500

static int core_el_any_connect_ptt_active(const media_core_t *core);
static void core_el_end_ysf_call(media_core_t *core);
static void core_el_dmr_end_call(media_core_t *core);

/* ---- shared helpers ---- */

static int pcm_rms16(const int16_t *pcm, int n)
{
    long long acc = 0;
    int i;

    if (n <= 0)
        return 0;
    for (i = 0; i < n; i++)
        acc += (long)pcm[i] * (long)pcm[i];
    return (int)sqrt((double)acc / (double)n);
}

static void pcm_apply_gain(int16_t *pcm, int n, float gain)
{
    int i;

    if (!pcm || n <= 0 || gain == 1.0f)
        return;
    for (i = 0; i < n; i++) {
        float v = (float)pcm[i] * gain;

        if (v > 32767.0f)
            v = 32767.0f;
        else if (v < -32768.0f)
            v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
}

static int core_el_vocoder_active(media_core_t *core)
{
    return core && core->use_vocoder && vocoder_is_ready(&core->voc);
}

static int core_el_pcm_to_ambe(media_core_t *core, const int16_t *pcm, uint8_t ambe[7])
{
    if (!core_el_vocoder_active(core))
        return -1;
    return vocoder_encode(&core->voc, pcm, ambe);
}

static int core_el_ambe_to_pcm(media_core_t *core, const uint8_t ambe[7], int16_t pcm[160])
{
    if (!core_el_vocoder_active(core))
        return -1;
    return vocoder_decode(&core->voc, ambe, pcm);
}

static int core_router_take(media_core_t *core, int peer_id)
{
    if (!core->router || peer_id < 0)
        return 1;
    if (!media_router_ingress_allowed(core->router, peer_id))
        return 0;
    media_router_ingress_begin(core->router, peer_id);
    return 1;
}

static int core_router_take_kind(media_core_t *core, media_peer_kind_t kind)
{
    int slot = core->router ? media_router_find_first(core->router, kind) : -1;

    return core_router_take(core, slot);
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

/* Best-known talker id for the currently active EL->DMR TX leg, else the
 * bridge peer's own id, else the bridge_dmrid fallback (valid even with no
 * live DMR peer, e.g. EL<->YSF layout — mirrors bridge_el_t.bridge_dmrid). */
static int core_el_rf_id_or_bridge(media_core_t *core, peer_dmr_t *dmr)
{
    if (core->leg_el_tx_dmr.call.talker_id > 0)
        return core->leg_el_tx_dmr.call.talker_id;
    if (dmr && dmr->dmrid > 0)
        return dmr->dmrid;
    return core->bridge_dmrid;
}

/* ---- talker identity (mirrors adapter_el_resolve_talker's two branches) ---- */

static void core_el_resolve_talker_for_dmr(media_core_t *core, peer_echolink_t *el)
{
    const char *raw = peer_el_remote_talker(el);
    char base[16];
    char talker10[10];
    int id = 0;
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);

    if (!raw || !raw[0])
        raw = el->callsign;
    identity_callsign_base(raw, base);
    if (!base[0]) {
        identity_callsign_base(el->callsign, base);
        raw = el->callsign;
    }

    identity_format_base_callsign10(talker10, base);
    id = identity_callsign10_to_dmrid((const uint8_t *)talker10);
    if (id <= 0 && base[0] && core->aliases)
        id = identity_lookup_alias_id(core->aliases, base);

    if (id > 0) {
        memcpy(core->leg_el_tx_dmr.call.netcall.net_src, talker10, 10);
        core->leg_el_tx_dmr.call.talker_id = id;
        LOG_DMR_INFO("%s talker %s -> id %d (alias)\n",
                     media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR), base, id);
        return;
    }
    if (dmr && dmr->dmrid > 0) {
        memcpy(core->leg_el_tx_dmr.call.netcall.net_src, dmr->callsign, 10);
        core->leg_el_tx_dmr.call.talker_id = dmr->dmrid;
        LOG_DMR_INFO("%s talker %s unknown -> bridge %.10s id %d\n",
                     media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR),
                     base[0] ? base : "?", core->leg_el_tx_dmr.call.netcall.net_src,
                     core->leg_el_tx_dmr.call.talker_id);
        return;
    }
    if (core->bridge_dmrid > 0) {
        core->leg_el_tx_dmr.call.talker_id = core->bridge_dmrid;
        LOG_DMR_INFO("%s talker %s unknown -> bridge id %d\n",
                     media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR),
                     base[0] ? base : "?", core->leg_el_tx_dmr.call.talker_id);
        return;
    }
    core->leg_el_tx_dmr.call.talker_id = 0;
    LOG_DMR_WARNING("%s talker %s: no DMR id (alias miss, no bridge dmrid)\n",
                    media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR),
                    base[0] ? base : "?");
}

static void core_el_resolve_talker_for_ysf(media_core_t *core, peer_echolink_t *el)
{
    const char *raw = peer_el_remote_talker(el);
    char base[16];
    char prev[10];
    int id = 0;

    if (!raw || !raw[0])
        raw = el->callsign;
    identity_callsign_base(raw, base);
    if (!base[0]) {
        identity_callsign_base(el->callsign, base);
        raw = el->callsign;
    }

    memcpy(prev, core->leg_el_tx_ysf.call.netcall.net_src, 10);
    identity_format_full_callsign10(core->leg_el_tx_ysf.call.netcall.net_src, raw);
    {
        char talker10[10];

        identity_format_base_callsign10(talker10, base);
        id = identity_callsign10_to_dmrid((const uint8_t *)talker10);
        if (id <= 0 && base[0] && core->aliases)
            id = identity_lookup_alias_id(core->aliases, base);
    }
    core->leg_el_tx_ysf.call.talker_id = id > 0 ? id : core->bridge_dmrid;
    if (memcmp(prev, core->leg_el_tx_ysf.call.netcall.net_src, 10) != 0)
        LOG_YSF_INFO("%s talker raw=%s base=%s\n",
                     media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_YSF),
                     raw, base[0] ? base : "?");
}

/* RX-from-YSF only: who's currently talking INTO EchoLink. */
static void core_el_set_ysf_talker_name(media_core_t *core, peer_echolink_t *el)
{
    char talker[16];
    char name[32];
    char label[32];

    identity_wire_call_to_cstr(talker, core->leg_el_rx.call.netcall.net_src);
    if (!talker[0]) {
        peer_el_set_talker_name(el, NULL);
        peer_el_set_relay_label(el, "YSF");
        return;
    }
    snprintf(name, sizeof(name), "%.10s (%.12s)", el->callsign, talker);
    peer_el_set_talker_name(el, name);
    snprintf(label, sizeof(label), "YSF %.12s", talker);
    peer_el_set_relay_label(el, label);
}

/* =====================================================================
 * EchoLink <-> DMR
 * ===================================================================== */

/* Wire-correct multi-destination fan-out: each DMR destination connection
 * gets its own seq/stream, not one shared across all of them (Fase 5). */
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

static void core_el_dmr_tx_dmrd(media_core_t *core, media_peer_slot_t *slot,
                                uint8_t frame_type, const uint8_t *voice33)
{
    dmr_tx_args_t args;
    peer_dmr_t *dmr;

    if (!slot || !slot->open)
        return;
    dmr = &slot->u.dmr;

    /* Same as core_ysf_dmr.c's core_dmr_tx_one(): the synthetic PTT to TG
     * 4000 (clear_dynamic_tg) must be a private (unit) call, not group. */
    if (slot->cp_active && slot->cp_clearing)
        frame_type |= DMRD_CALL_PRIVATE;

    args.peer = dmr;
    args.bridge_dmrid = dmr->dmrid;
    args.talker_rf_id = core_el_rf_id_or_bridge(core, dmr);
    args.tx_tg = (slot->cp_active && slot->cp_tg > 0) ? slot->cp_tg : dmr->tg;
    args.seq = &slot->dmr_tx_seq;
    args.stream_id = slot->dmr_tx_stream_id;
    args.last_tx = &core->leg_el_tx_dmr.last_dmr_tx;
    args.emb_raw = slot->dmr_emb_raw;
    adapter_dmr_egress_dmrd(&args, frame_type, voice33);
}

typedef struct {
    media_core_t  *core;
    uint8_t        frame_type;
    const uint8_t *voice33;
} core_el_fanout_dmrd_ctx_t;

static int core_el_fanout_dmrd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    core_el_fanout_dmrd_ctx_t *ctx = vctx;
    media_peer_slot_t *slot;

    if (kind != MEDIA_PEER_DMR)
        return 0;
    slot = media_peer_bus_slot_mut(ctx->core->bus, dst_id);
    if (!slot)
        return 0;
    core_el_dmr_tx_dmrd(ctx->core, slot, ctx->frame_type, ctx->voice33);
    return 0;
}

static void core_el_send_dmrd(media_core_t *core, uint8_t frame_type, const uint8_t *voice33)
{
    core_el_fanout_dmrd_ctx_t ctx = { core, frame_type, voice33 };
    int src = media_router_find_first(core->router, MEDIA_PEER_ECHOLINK);

    if (core->router && src >= 0)
        media_router_fanout(core->router, src, core_el_fanout_dmrd_cb, &ctx);
}

static void core_el_dmr_emit_voice(media_core_t *core, const uint8_t voice33[33])
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
    uint8_t slot_bit = core->dmr_slot_bit;
    uint8_t n = (uint8_t)(core->leg_el_tx_dmr.dmr_voice_frames % 6);
    uint8_t b15;

    if (core->leg_el_tx_dmr.phase == MEDIA_CALL_IDLE) {
        if (!core_router_take_kind(core, MEDIA_PEER_ECHOLINK))
            return;
        core->leg_el_tx_dmr.phase = MEDIA_CALL_TX_TO_PEER;
        core->leg_el_tx_dmr.call.stream_id = bridge_new_stream_id();
        core->leg_el_tx_dmr.dmr_seq = 0;
        core->leg_el_tx_dmr.dmr_voice_frames = 0;
        core_reset_dmr_tx_slots(core);
        modeconv_reset(core->mc_el_dmr);
        core_el_resolve_talker_for_dmr(core, el);
        /* One VHEAD only: identical repeats are counted as loss (dup CRC /
         * lastData) by adn-server PacketControl and create SEQ gaps. */
        core_el_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                          NULL);
        if (el)
            el->rtp_rx_packets = 0;
        LOG_DMR_INFO("%s call start (TG %d, src %.10s id %d)\n",
                     media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR),
                     dmr ? dmr->tg : 0, core->leg_el_tx_dmr.call.netcall.net_src,
                     core->leg_el_tx_dmr.call.talker_id);
    }

    b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
    core_el_send_dmrd(core, b15, voice33);
    core->leg_el_tx_dmr.dmr_voice_frames++;
}

/* Begin paced teardown: pad to superframe then VTERM at DMR_FRAME_MS.
 * Bursting pads+VTERM in one tick was counted as SEQ/rate stress on short calls. */
static void core_el_dmr_begin_end(media_core_t *core)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);

    if (core->leg_el_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER || core->leg_el_tx_dmr.dmr_ending)
        return;
    core->leg_el_tx_dmr.dmr_ending = 1;
    core->leg_el_tx_dmr.el_ambe_count = 0;
    core->leg_el_capture.pcm_el_acc_n = 0;
    if (el)
        peer_el_drop_pcm_in(el);
    modeconv_reset(core->mc_el_dmr);
    /* Allow first pad/VTERM on the next tick immediately. */
    core->leg_el_tx_dmr.last_dmr_tx.tv_sec = 0;
    core->leg_el_tx_dmr.last_dmr_tx.tv_nsec = 0;
}

static void core_el_dmr_finish_end(media_core_t *core)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);

    LOG_DMR_INFO("%s call end (%d DMR frames out, el_rtp_rx=%u, seq=%u)\n",
                 media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR),
                 core->leg_el_tx_dmr.dmr_voice_frames, el ? el->rtp_rx_packets : 0,
                 (unsigned)core->leg_el_tx_dmr.dmr_seq);
    core_router_release_active(core);
    core->leg_el_tx_dmr.phase = MEDIA_CALL_IDLE;
    core->leg_el_tx_dmr.dmr_ending = 0;
    core->leg_el_tx_dmr.dmr_voice_frames = 0;
    core->leg_el_tx_dmr.el_ambe_count = 0;
    core->leg_el_tx_dmr.el_speech_run = 0;
    core->leg_el_tx_dmr.call.talker_id = 0;
    if (el) {
        peer_el_drop_pcm_in(el);
        peer_el_clear_remote_talker(el);
    }
    modeconv_reset(core->mc_el_dmr);
    bridge_stamp_now(&core->leg_el_tx_dmr.last_el_tx_end);
}

static void core_el_dmr_pace_end(media_core_t *core)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    if (!core->leg_el_tx_dmr.dmr_ending || core->leg_el_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER)
        return;
    if (!bridge_ms_elapsed(&core->leg_el_tx_dmr.last_dmr_tx, DMR_FRAME_MS))
        return;

    if ((core->leg_el_tx_dmr.dmr_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(core->leg_el_tx_dmr.dmr_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        core_el_send_dmrd(core, b15, DMR_SILENCE_DATA);
        core->leg_el_tx_dmr.dmr_voice_frames++;
        return;
    }
    core_el_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                      DMR_SILENCE_DATA);
    core_el_dmr_finish_end(core);
}

/* Immediate VTERM (no pad burst) — used when DMR RX preempts EL TX. */
static void core_el_dmr_end_call(media_core_t *core)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    if (core->leg_el_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER && !core->leg_el_tx_dmr.dmr_ending)
        return;
    if (core->leg_el_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
        core_el_send_dmrd(core, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                          DMR_SILENCE_DATA);
    core_el_dmr_finish_end(core);
}

/* Accumulate one AMBE frame toward the next DMR voice33 (group of 3),
 * flushing to mc_el_dmr when full. Independent of the YSF feed below —
 * both may run for the same underlying 160-sample chunk (Fase 8). */
static void core_el_dmr_feed_ambe(media_core_t *core, int rms, const uint8_t ambe[7])
{
    int in_cooldown = (core->leg_el_tx_dmr.last_el_tx_end.tv_sec || core->leg_el_tx_dmr.last_el_tx_end.tv_nsec)
                      && bridge_ms_since(&core->leg_el_tx_dmr.last_el_tx_end) < EL_TX_COOLDOWN_MS;

    if (core->leg_el_tx_dmr.phase == MEDIA_CALL_IDLE) {
        if (in_cooldown) {
            static int drop_dbg;
            if (++drop_dbg <= 3 || (drop_dbg % 50) == 0)
                LOG_EL_DEBUG("echolink: post-TX cooldown drop rms=%d\n", rms);
            core->leg_el_tx_dmr.el_ambe_count = 0;
            core->leg_el_tx_dmr.el_speech_run = 0;
            return;
        }
        if (!core->leg_el_tx_dmr.el_speech_run) {
            LOG_EL_INFO("echolink: EL audio rms=%d — starting EL TX path (TG activate)\n", rms);
            core->leg_el_tx_dmr.el_speech_run = 1;
        }
    }

    memcpy(core->leg_el_tx_dmr.el_ambe_buf[core->leg_el_tx_dmr.el_ambe_count], ambe, 7);
    core->leg_el_tx_dmr.el_ambe_count++;
    if (core->leg_el_tx_dmr.el_ambe_count < 3)
        return;

    modeconv_put_ambe7(core->mc_el_dmr, core->leg_el_tx_dmr.el_ambe_buf[0]);
    modeconv_put_ambe7(core->mc_el_dmr, core->leg_el_tx_dmr.el_ambe_buf[1]);
    modeconv_put_ambe7(core->mc_el_dmr, core->leg_el_tx_dmr.el_ambe_buf[2]);
    core->leg_el_tx_dmr.el_ambe_count = 0;
}

void core_el_dmr_ingress_dmr(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);
    uint8_t ambe[3][7];
    int16_t pcm[160];
    int i;

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN: {
        uint32_t sid = frame->meta.stream_id;

        /* Duplicate VHEAD on the active stream — do not reset counters/RTP. */
        if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER && sid == core->leg_el_rx.dmr_rx_stream_id) {
            bridge_stamp_now(&core->leg_el_rx.last_dmr_rx);
            return;
        }
        /* A human can only listen to one source — end whichever EL->network
         * TX pathway (DMR, YSF, or both) is currently sending before we
         * claim the leg for incoming DMR audio. */
        if (core->leg_el_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
            core_el_dmr_end_call(core);
        if (core->leg_el_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
            core_el_end_ysf_call(core);
        if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER) {
            if (el) {
                peer_el_flush_pcm(el);
                peer_el_set_relay_label(el, NULL);
            }
            LOG_DMR_INFO("%s call end (%d voice frames in, el_rtp_tx=%u) — replaced by new stream\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.dmr_voice_frames, el ? el->rtp_tx_packets : 0);
            core_router_release_active(core);
        }
        if (!core_router_take(core, src_router_id))
            return;
        core->leg_el_rx.phase = MEDIA_CALL_RX_FROM_PEER;
        core->leg_el_rx.rx_src_kind = MEDIA_PEER_DMR;
        core->leg_el_rx.dmr_rx_stream_id = sid;
        core->leg_el_rx.dmr_voice_frames = 0;
        if (el) {
            char cs[16];
            char label[32];

            el->rtp_tx_packets = 0;
            identity_dmr_display_callsign(core->aliases, frame->meta.talker_id, cs);
            snprintf(label, sizeof(label), cs[0] ? "DMR %s" : "DMR", cs);
            peer_el_set_relay_label(el, label);
        }
        bridge_stamp_now(&core->leg_el_rx.last_dmr_rx);
        LOG_DMR_INFO("%s call start\n", media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_ECHOLINK));
        return;
    }
    case MEDIA_FRAME_CALL_END:
        if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER && core->leg_el_rx.rx_src_kind == MEDIA_PEER_DMR) {
            if (el) {
                peer_el_flush_pcm(el);
                peer_el_set_relay_label(el, NULL);
            }
            LOG_DMR_INFO("%s call end (%d voice frames in, el_rtp_tx=%u)\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.dmr_voice_frames, el ? el->rtp_tx_packets : 0);
            core_router_release_active(core);
            core->leg_el_rx.phase = MEDIA_CALL_IDLE;
            core->leg_el_rx.dmr_voice_frames = 0;
            core->leg_el_rx.dmr_rx_stream_id = 0;
        }
        return;
    case MEDIA_FRAME_VOICE:
        if (core->leg_el_rx.phase != MEDIA_CALL_RX_FROM_PEER || core->leg_el_rx.rx_src_kind != MEDIA_PEER_DMR)
            return;
        if (!core->use_vocoder)
            return;
        (void)src_router_id;
        bridge_stamp_now(&core->leg_el_rx.last_dmr_rx);
        core->leg_el_rx.dmr_voice_frames++;
        modeconv_dmr33_to_ambe(frame->payload.dmr_voice33, ambe);
        for (i = 0; i < 3; i++) {
            if (core_el_ambe_to_pcm(core, ambe[i], pcm) != 0) {
                static int voc_fail;
                if (bridge_dbg_periodic(&voc_fail))
                    LOG_DMR_WARNING("%s vocoder decode failed\n",
                                    media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_ECHOLINK));
                continue;
            }
            if (el)
                adapter_el_egress_pcm(el, pcm, 160);
        }
        return;
    default:
        return;
    }
}

/* ---- connect-PTT (DMR login rising edge) — per DMR destination slot, since
 * each may have its own TG and its own clear_dynamic_tg config (Fase 5-style
 * fix: one shared TG fanned out to every destination was wrong the moment a
 * second DMR peer with a different TG was configured). Duplicated from
 * core_ysf_dmr.c on purpose, matching bridge_el.c's existing duplication of
 * bridge.c's logic; see docs-priv/media-bus-dumb-modes-plan.md correction #2. */

static void core_el_cp_begin_stream(media_peer_slot_t *slot, int tg, int clearing)
{
    slot->cp_active = 1;
    slot->cp_phase = 0;
    slot->cp_voice_frames = 0;
    slot->cp_tg = tg;
    slot->cp_clearing = clearing ? 1 : 0;
    slot->dmr_tx_stream_id = bridge_new_stream_id();
    slot->dmr_tx_seq = 0;
    bridge_stamp_now(&slot->cp_start);
    LOG_DMR_INFO("DMR connect PTT start (TG %d, %d ms)%s [%.10s]\n",
                 tg, CONNECT_PTT_MS, clearing ? " [clear dynamic]" : "", slot->u.dmr.callsign);
}

/* Silent placeholder phase -- no TX, just waiting out a fixed delay before
 * the next real PTT. cp_tg/cp_clearing are stashed here as "what to send
 * once the wait ends" (core_el_cp_begin_stream reads them back). Used for
 * both the initial post-connect delay and the post-4000 gap. */
static void core_el_cp_begin_wait(media_peer_slot_t *slot, int phase, int next_tg, int next_clearing)
{
    slot->cp_active = 1;
    slot->cp_phase = phase;
    slot->cp_voice_frames = 0;
    slot->cp_tg = next_tg;
    slot->cp_clearing = next_clearing ? 1 : 0;
    bridge_stamp_now(&slot->cp_start);
}

static void core_el_cp_finish(media_core_t *core, media_peer_slot_t *slot)
{
    uint8_t slot_bit = core->dmr_slot_bit;
    peer_dmr_t *dmr = &slot->u.dmr;
    int ended_tg = slot->cp_tg > 0 ? slot->cp_tg : dmr->tg;
    int was_clearing = slot->cp_clearing;

    while ((slot->cp_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(slot->cp_voice_frames % 6);
        core_el_dmr_tx_dmrd(core, slot, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        slot->cp_voice_frames++;
    }
    core_el_dmr_tx_dmrd(core, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                       DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames) [%.10s]\n",
                 ended_tg, slot->cp_voice_frames, dmr->callsign);

    if (was_clearing && dmr->tg > 0) {
        core_el_cp_begin_wait(slot, 2, dmr->tg, 0);
        return;
    }
    slot->cp_active = 0;
    slot->cp_phase = 0;
    slot->cp_voice_frames = 0;
    slot->cp_tg = 0;
    slot->cp_clearing = 0;
}

static void core_el_abort_el_to_dmr(media_core_t *core)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);

    /* Silent abort on DMR drop — do not emit VTERM into a dead/reconnecting session. */
    if (core->leg_el_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER && !core->leg_el_tx_dmr.dmr_ending)
        return;
    LOG_DMR_INFO("%s aborted — DMR peer down (was phase=%d ending=%d)\n",
                 media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR),
                 core->leg_el_tx_dmr.phase, core->leg_el_tx_dmr.dmr_ending);
    core_router_release_active(core);
    core->leg_el_tx_dmr.phase = MEDIA_CALL_IDLE;
    core->leg_el_tx_dmr.dmr_ending = 0;
    core->leg_el_tx_dmr.dmr_voice_frames = 0;
    core->leg_el_tx_dmr.el_ambe_count = 0;
    core->leg_el_tx_dmr.el_speech_run = 0;
    core->leg_el_tx_dmr.call.talker_id = 0;
    if (el) {
        peer_el_drop_pcm_in(el);
        peer_el_clear_remote_talker(el);
    }
    modeconv_reset(core->mc_el_dmr);
    bridge_stamp_now(&core->leg_el_tx_dmr.last_el_tx_end);
}

static void core_el_start_connect_ptt(media_peer_slot_t *slot, media_call_phase_t phase)
{
    peer_dmr_t *dmr = &slot->u.dmr;

    if (phase != MEDIA_CALL_IDLE || slot->cp_active)
        return;
    if (dmr->tg <= 0)
        return;
    if (slot->clear_dynamic_tg)
        core_el_cp_begin_wait(slot, 3, DMR_CLEAR_DYNAMIC_TG, 1);
    else
        core_el_cp_begin_wait(slot, 3, dmr->tg, 0);
}

static void core_el_emit_connect_ptt(media_core_t *core, media_peer_slot_t *slot)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    if (!slot->cp_active)
        return;

    if (slot->cp_phase == 3) {
        if (bridge_ms_since(&slot->cp_start) >= CONNECT_PTT_START_DELAY_MS)
            core_el_cp_begin_stream(slot, slot->cp_tg, slot->cp_clearing);
        return;
    }
    if (slot->cp_phase == 2) {
        if (bridge_ms_since(&slot->cp_start) >= CONNECT_PTT_GAP_MS)
            core_el_cp_begin_stream(slot, slot->cp_tg, slot->cp_clearing);
        return;
    }
    if (slot->cp_phase == 0) {
        core_el_dmr_tx_dmrd(core, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                           NULL);
        slot->cp_phase = 1;
        bridge_stamp_now(&slot->cp_start);
        return;
    }
    if (bridge_ms_since(&slot->cp_start) >= CONNECT_PTT_MS) {
        core_el_cp_finish(core, slot);
        return;
    }
    {
        uint8_t n = (uint8_t)(slot->cp_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
        core_el_dmr_tx_dmrd(core, slot, b15, DMR_SILENCE_DATA);
        slot->cp_voice_frames++;
    }
}

static void core_el_dmr_poll_connect_ptt(media_core_t *core)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];
        int connected;

        if (slot->kind != MEDIA_PEER_DMR || !slot->open)
            continue;
        connected = peer_dmr_connected(&slot->u.dmr);
        if (connected && !slot->dmr_was_connected)
            core_el_start_connect_ptt(slot, core->leg_el_tx_dmr.phase);
        if (!connected) {
            slot->cp_active = 0;
            slot->cp_phase = 0;
            slot->cp_voice_frames = 0;
            slot->cp_tg = 0;
            slot->cp_clearing = 0;
            core_el_abort_el_to_dmr(core);
        }
        slot->dmr_was_connected = connected;

        if (slot->cp_active && bridge_ms_elapsed(&core->leg_el_tx_dmr.last_dmr_tx, DMR_FRAME_MS))
            core_el_emit_connect_ptt(core, slot);
    }
}

static int core_el_any_connect_ptt_active(const media_core_t *core)
{
    int i;

    if (!core->bus)
        return 0;
    for (i = 0; i < core->bus->n_slots; i++) {
        if (core->bus->slots[i].kind == MEDIA_PEER_DMR && core->bus->slots[i].cp_active)
            return 1;
    }
    return 0;
}

static void core_el_dmr_pace_tx(media_core_t *core)
{
    uint8_t voice33[33];

    if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER || core_el_any_connect_ptt_active(core)
        || core->leg_el_tx_dmr.dmr_ending)
        return;
    if (core->leg_el_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER
        && !bridge_ms_elapsed(&core->leg_el_tx_dmr.last_dmr_tx, DMR_FRAME_MS))
        return;
    if (modeconv_get_dmr(core->mc_el_dmr, voice33) == MODECONV_TAG_DATA)
        core_el_dmr_emit_voice(core, voice33);
}

/* =====================================================================
 * EchoLink <-> YSF (framing mirrors adapters/dmr.c+ysf.c's DMR2YSF path)
 * ===================================================================== */

static int core_el_tx_ysfd(media_core_t *core, peer_ysf_t *ysf, uint8_t fi, uint8_t ft,
                           uint8_t cm, uint8_t fich_fn, uint8_t net_cnt,
                           const uint8_t *payload120, const uint8_t csd1[20], const uint8_t csd2[20])
{
    ysf_tx_args_t args;

    if (!ysf)
        return 0;
    args = (ysf_tx_args_t){
        .peer = ysf,
        .repeater_callsign = ysf->callsign,
        .meta = &core->leg_el_tx_ysf.call.netcall,
        .last_tx = &core->leg_el_tx_ysf.last_ysf_tx,
        .dgid_cfg = ysf->dgid,
    };
    return adapter_ysf_egress_ysfd(&args, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2);
}

typedef struct {
    media_core_t  *core;
    uint8_t        fi, ft, cm, fich_fn, net_cnt;
    const uint8_t *payload120, *csd1, *csd2;
} core_el_fanout_ysfd_ctx_t;

static int core_el_fanout_ysfd_cb(int dst_id, media_peer_kind_t kind, void *vctx)
{
    core_el_fanout_ysfd_ctx_t *ctx = vctx;
    peer_ysf_t *ysf;

    if (kind != MEDIA_PEER_YSF)
        return 0;
    ysf = media_peer_bus_ysf(ctx->core->bus, dst_id);
    if (!ysf)
        return 0;
    core_el_tx_ysfd(ctx->core, ysf, ctx->fi, ctx->ft, ctx->cm, ctx->fich_fn, ctx->net_cnt,
                    ctx->payload120, ctx->csd1, ctx->csd2);
    return 0;
}

static void core_el_send_ysfd(media_core_t *core, uint8_t fi, uint8_t ft, uint8_t cm,
                              uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                              const uint8_t csd1[20], const uint8_t csd2[20])
{
    core_el_fanout_ysfd_ctx_t ctx = { core, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2 };
    int src = media_router_find_first(core->router, MEDIA_PEER_ECHOLINK);

    if (core->router && src >= 0)
        media_router_fanout(core->router, src, core_el_fanout_ysfd_cb, &ctx);
}

static int core_el_emit_ysf_from_conv(media_core_t *core)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);
    uint8_t payload[120];
    unsigned int tag;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(core->mc_el, payload);
    if (tag == MODECONV_TAG_NODATA)
        return 0;

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        core->leg_el_tx_ysf.ysf_cnt = 0;
        ysf_tx_fill_csd(&core->leg_el_tx_ysf.call.netcall, csd1, csd2);
        core_el_send_ysfd(core, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        core->leg_el_tx_ysf.ysf_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        ysf_tx_fill_csd(&core->leg_el_tx_ysf.call.netcall, csd1, csd2);
        core_el_send_ysfd(core, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                          core->leg_el_tx_ysf.ysf_cnt, NULL, csd1, csd2);
        LOG_YSF_INFO("%s call end (%d voice frames out, el_rtp_rx=%u)\n",
                     media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_YSF),
                     core->leg_el_tx_ysf.ysf_voice_frames, el ? el->rtp_rx_packets : 0);
        core_router_release_active(core);
        core->leg_el_tx_ysf.phase = MEDIA_CALL_IDLE;
        core->leg_el_tx_ysf.ysf_ending = 0;
        core->leg_el_tx_ysf.ysf_voice_frames = 0;
        core->leg_el_tx_ysf.ysf_ambe_count = 0;
        core->leg_el_tx_ysf.ysf_cnt = 0;
        core->leg_el_tx_ysf.el_speech_run = 0;
        core->leg_el_tx_ysf.call.talker_id = 0;
        if (el) {
            peer_el_drop_pcm_in(el);
            peer_el_clear_remote_talker(el);
        }
        modeconv_reset(core->mc_el);
        bridge_stamp_now(&core->leg_el_tx_ysf.last_el_tx_end);
        return 1;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((core->leg_el_tx_ysf.ysf_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((core->leg_el_tx_ysf.ysf_cnt & 0x7FU) << 1);

        core_el_send_ysfd(core, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM, fn, net,
                          payload, NULL, NULL);
        core->leg_el_tx_ysf.ysf_voice_frames++;
        core->leg_el_tx_ysf.ysf_cnt++;
        return 1;
    }
    return 0;
}

static void core_el_end_ysf_call(media_core_t *core)
{
    if (core->leg_el_tx_ysf.phase != MEDIA_CALL_TX_TO_PEER || core->leg_el_tx_ysf.ysf_ending)
        return;
    /* Same as DMR->YSF: queue ModeConv EOT; HEADER/CSD/EOT leave on paced emit. */
    core->leg_el_tx_ysf.ysf_ending = 1;
    modeconv_put_dmr_eot(core->mc_el);
}

static void core_el_begin_el_to_ysf(media_core_t *core)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);

    core_el_resolve_talker_for_ysf(core, el);
    memset(core->leg_el_tx_ysf.call.netcall.net_dst, ' ', 10);
    memcpy(core->leg_el_tx_ysf.call.netcall.net_dst, YSF_WIRE_DST_ALL, 10);
    if (!core_router_take_kind(core, MEDIA_PEER_ECHOLINK))
        return;
    core->leg_el_tx_ysf.phase = MEDIA_CALL_TX_TO_PEER;
    core->leg_el_tx_ysf.ysf_ending = 0;
    core->leg_el_tx_ysf.ysf_voice_frames = 0;
    core->leg_el_tx_ysf.ysf_ambe_count = 0;
    core->leg_el_tx_ysf.ysf_cnt = 0;
    modeconv_reset(core->mc_el);
    modeconv_put_dmr_header(core->mc_el);
    if (el)
        el->rtp_rx_packets = 0;
    LOG_YSF_INFO("%s call start (src %.10s)\n",
                 media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_YSF), core->leg_el_tx_ysf.call.netcall.net_src);
}

/* Late SDES user talker: radios lock HEADER — re-queue HEADER with new CSD. */
static void core_el_ysf_reheader_if_talker_changed(media_core_t *core, peer_echolink_t *el)
{
    const char *raw;
    char want[10];
    char prev[10];

    if (core->leg_el_tx_ysf.phase != MEDIA_CALL_TX_TO_PEER || core->leg_el_tx_ysf.ysf_ending)
        return;
    raw = peer_el_remote_talker(el);
    if (!raw || !raw[0])
        return;
    identity_format_full_callsign10(want, raw);
    if (memcmp(want, core->leg_el_tx_ysf.call.netcall.net_src, 10) == 0)
        return;
    memcpy(prev, core->leg_el_tx_ysf.call.netcall.net_src, 10);
    core_el_resolve_talker_for_ysf(core, el);
    if (memcmp(prev, core->leg_el_tx_ysf.call.netcall.net_src, 10) == 0)
        return;
    modeconv_put_dmr_header(core->mc_el);
    LOG_YSF_INFO("%s re-HEADER talker %.10s -> %.10s\n",
                 media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_YSF), prev,
                 core->leg_el_tx_ysf.call.netcall.net_src);
}

static void core_el_drain_ysf_to_el_pcm(media_core_t *core, peer_echolink_t *el)
{
    uint8_t voice33[33];
    uint8_t ambe[3][7];
    int16_t pcm[160];
    unsigned int tag;
    int i;

    while ((tag = modeconv_get_dmr(core->mc_el, voice33)) != MODECONV_TAG_NODATA) {
        if (tag != MODECONV_TAG_DATA)
            continue;
        modeconv_dmr33_to_ambe(voice33, ambe);
        for (i = 0; i < 3; i++) {
            if (core_el_ambe_to_pcm(core, ambe[i], pcm) != 0) {
                static int voc_fail;
                if (bridge_dbg_periodic(&voc_fail))
                    LOG_YSF_WARNING("%s vocoder decode failed\n",
                                    media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK));
                continue;
            }
            if (el)
                adapter_el_egress_pcm(el, pcm, 160);
        }
    }
}

/* Accumulate one AMBE frame toward the next YSFD payload (group of 5),
 * flushing to mc_el when full. Independent of the DMR feed above. */
static void core_el_ysf_feed_ambe(media_core_t *core, int rms, const uint8_t ambe[7])
{
    int i;
    int in_cooldown = (core->leg_el_tx_ysf.last_el_tx_end.tv_sec || core->leg_el_tx_ysf.last_el_tx_end.tv_nsec)
                      && bridge_ms_since(&core->leg_el_tx_ysf.last_el_tx_end) < EL_TX_COOLDOWN_MS;

    if (core->leg_el_tx_ysf.phase == MEDIA_CALL_IDLE) {
        if (in_cooldown) {
            static int drop_dbg;
            if (++drop_dbg <= 3 || (drop_dbg % 50) == 0)
                LOG_EL_DEBUG("echolink: post-TX cooldown drop rms=%d (YSF)\n", rms);
            core->leg_el_tx_ysf.ysf_ambe_count = 0;
            core->leg_el_tx_ysf.el_speech_run = 0;
            return;
        }
        if (!core->leg_el_tx_ysf.el_speech_run) {
            LOG_EL_INFO("echolink: audio rms=%d — starting %s path\n", rms,
                        media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_YSF));
            core->leg_el_tx_ysf.el_speech_run = 1;
        }
        core_el_begin_el_to_ysf(core);
    }

    memcpy(core->leg_el_tx_ysf.ysf_ambe_buf[core->leg_el_tx_ysf.ysf_ambe_count], ambe, 7);
    core->leg_el_tx_ysf.ysf_ambe_count++;
    if (core->leg_el_tx_ysf.ysf_ambe_count < 5)
        return;

    /* Queue only — YSFD HEADER/VOICE/EOT leave via paced emit (DMR->YSF). */
    for (i = 0; i < 5; i++)
        modeconv_put_ambe7_ysf(core->mc_el, core->leg_el_tx_ysf.ysf_ambe_buf[i]);
    core->leg_el_tx_ysf.ysf_ambe_count = 0;
}

void core_el_ysf_ingress_ysf(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);
    uint8_t scratch120[120];

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN:
        if (core->leg_el_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
            core_el_end_ysf_call(core);
        if (core->leg_el_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
            core_el_dmr_end_call(core);
        if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER) {
            if (el) {
                peer_el_flush_pcm(el);
                peer_el_set_talker_name(el, NULL);
                peer_el_set_relay_label(el, NULL);
            }
            LOG_YSF_INFO("%s call end (%d voice frames in, el_rtp_tx=%u) — replaced by new stream\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.ysf_voice_frames, el ? el->rtp_tx_packets : 0);
            core_router_release_active(core);
        }
        core->leg_el_rx.call.netcall = frame->meta.netcall;
        memset(core->leg_el_rx.call.netcall.net_dst, ' ', 10);
        memcpy(core->leg_el_rx.call.netcall.net_dst, YSF_WIRE_DST_ALL, 10);
        if (!core_router_take(core, src_router_id))
            return;
        core->leg_el_rx.phase = MEDIA_CALL_RX_FROM_PEER;
        core->leg_el_rx.rx_src_kind = MEDIA_PEER_YSF;
        core->leg_el_rx.ysf_voice_frames = 0;
        if (el)
            el->rtp_tx_packets = 0;
        bridge_stamp_now(&core->leg_el_rx.last_dmr_rx); /* reuse: last peer->EL activity */
        modeconv_reset(core->mc_el);
        modeconv_put_ysf_header(core->mc_el);
        if (el)
            core_el_set_ysf_talker_name(core, el);
        LOG_YSF_INFO("%s call start (src %.10s)\n",
                     media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK), core->leg_el_rx.call.netcall.net_src);
        return;
    case MEDIA_FRAME_CALL_END:
        if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER && core->leg_el_rx.rx_src_kind == MEDIA_PEER_YSF) {
            modeconv_put_ysf_eot(core->mc_el);
            core_el_drain_ysf_to_el_pcm(core, el);
            if (el) {
                peer_el_flush_pcm(el);
                peer_el_set_talker_name(el, NULL);
                peer_el_set_relay_label(el, NULL);
            }
            LOG_YSF_INFO("%s call end (%d voice frames in, el_rtp_tx=%u)\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.ysf_voice_frames, el ? el->rtp_tx_packets : 0);
            core_router_release_active(core);
            core->leg_el_rx.phase = MEDIA_CALL_IDLE;
            core->leg_el_rx.ysf_voice_frames = 0;
            modeconv_reset(core->mc_el);
        }
        return;
    case MEDIA_FRAME_VOICE:
        if (core->leg_el_rx.phase != MEDIA_CALL_RX_FROM_PEER || core->leg_el_rx.rx_src_kind != MEDIA_PEER_YSF) {
            /* Late join without HEADER — start on first voice. */
            if (core->leg_el_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
                core_el_end_ysf_call(core);
            if (core->leg_el_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
                core_el_dmr_end_call(core);
            core->leg_el_rx.call.netcall = frame->meta.netcall;
            if (!core_router_take(core, src_router_id))
                return;
            core->leg_el_rx.phase = MEDIA_CALL_RX_FROM_PEER;
            core->leg_el_rx.rx_src_kind = MEDIA_PEER_YSF;
            core->leg_el_rx.ysf_voice_frames = 0;
            if (el)
                el->rtp_tx_packets = 0;
            modeconv_reset(core->mc_el);
            modeconv_put_ysf_header(core->mc_el);
            if (el)
                core_el_set_ysf_talker_name(core, el);
            LOG_YSF_INFO("%s call start (src %.10s, no HEADER)\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.call.netcall.net_src);
        }
        bridge_stamp_now(&core->leg_el_rx.last_dmr_rx);
        memcpy(scratch120, frame->payload.ysf_payload120, 120);
        modeconv_put_ysf_payload(core->mc_el, scratch120);
        core->leg_el_rx.ysf_voice_frames++;
        core_el_drain_ysf_to_el_pcm(core, el);
        return;
    default:
        return;
    }
}

/* =====================================================================
 * EchoLink mic capture -- one vocoder-encode per 160-sample chunk, fanned to
 * whichever of {DMR TX, YSF TX} pathways are currently eligible (Fase 8: the
 * two used to drain the same PCM ring buffer independently, so whichever ran
 * first — always DMR, by call order — silently starved the other every
 * tick). Reading/encoding happens exactly once per chunk regardless of how
 * many destinations consume it.
 * ===================================================================== */

void core_el_process_el_audio(media_core_t *core)
{
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
    peer_ysf_t *ysf = media_peer_bus_primary_ysf(core->bus);
    int16_t pcm[160];
    uint8_t ambe[7];
    int n;
    int enc_fail_streak = 0;
    int dmr_ok, ysf_ok;

    if (!el || !core->use_vocoder)
        return;
    if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER)
        return; /* network source has the slot (half-duplex) */
    /* core_el_dmr_pace_tx stops draining core->mc_el_dmr's ring buffer during
     * a connect-PTT burst; if we kept pushing AMBE in while that drain is
     * stalled, the buffer fills and overflows (silently desyncing ModeConv's
     * internal frame counter from the ring buffer's real contents, which
     * then misreports spurious underflows long after — a real bug seen in
     * production, inherited unchanged from v0.3.1's bridge_el.c). EL's own
     * jitter buffer (peer_echolink_t.pcm_in, 2s capacity) comfortably
     * absorbs skipping reads for one ~500ms CONNECT_PTT_MS window. This
     * guards the whole capture (not just the DMR feed below) since a stalled
     * mc_el_dmr drain would otherwise still let the YSF feed race ahead. */
    if (core_el_any_connect_ptt_active(core))
        return;

    /* SDES talker often arrives after first RTP — re-HEADER so radios update. */
    if (ysf && core->leg_el_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
        core_el_ysf_reheader_if_talker_changed(core, el);

    dmr_ok = dmr && peer_dmr_connected(dmr) && !core->leg_el_tx_dmr.dmr_ending;
    ysf_ok = ysf != NULL && !core->leg_el_tx_ysf.ysf_ending;
    if (!dmr_ok && !ysf_ok)
        return;

    while ((n = adapter_el_read_pcm(el, pcm, 160)) > 0) {
        int i;
        int rms;

        for (i = 0; i < n && core->leg_el_capture.pcm_el_acc_n < 160; i++)
            core->leg_el_capture.pcm_el_acc[core->leg_el_capture.pcm_el_acc_n++] = pcm[i];
        if (core->leg_el_capture.pcm_el_acc_n < 160)
            continue;

        rms = pcm_rms16(core->leg_el_capture.pcm_el_acc, 160);
        bridge_stamp_now(&core->leg_el_capture.last_el_speech);

        pcm_apply_gain(core->leg_el_capture.pcm_el_acc, 160, core->el_pcm_gain);
        if (core_el_pcm_to_ambe(core, core->leg_el_capture.pcm_el_acc, ambe) != 0) {
            static int voc_enc_fail;

            core->leg_el_capture.pcm_el_acc_n = 0;
            enc_fail_streak++;
            if (bridge_dbg_periodic(&voc_enc_fail))
                LOG_EL_WARNING("echolink: vocoder encode failed\n");
            if (enc_fail_streak >= 3) {
                while (adapter_el_read_pcm(el, pcm, 160) > 0)
                    ;
                core->leg_el_capture.pcm_el_acc_n = 0;
                core->leg_el_tx_dmr.el_ambe_count = 0;
                core->leg_el_tx_ysf.ysf_ambe_count = 0;
                break;
            }
            continue;
        }
        enc_fail_streak = 0;
        core->leg_el_capture.pcm_el_acc_n = 0;

        if (dmr_ok)
            core_el_dmr_feed_ambe(core, rms, ambe);
        if (ysf_ok)
            core_el_ysf_feed_ambe(core, rms, ambe);
    }
}

/* =====================================================================
 * Tick: pace/hang for whichever EL pathway(s) are active.
 * ===================================================================== */

void core_el_tick(media_core_t *core)
{
    peer_dmr_t *dmr;
    peer_echolink_t *el = media_peer_bus_primary_el(core->bus);
    int pairs_with_dmr = core->router && media_router_find_first(core->router, MEDIA_PEER_DMR) >= 0;
    int pairs_with_ysf = core->router && media_router_find_first(core->router, MEDIA_PEER_YSF) >= 0;

    if (pairs_with_dmr) {
        core_el_dmr_poll_connect_ptt(core);
        dmr = media_peer_bus_primary_dmr(core->bus);
        if (dmr && peer_dmr_connected(dmr)) {
            if (core->leg_el_tx_dmr.dmr_ending)
                core_el_dmr_pace_end(core);
            else
                core_el_dmr_pace_tx(core);
        }
    }

    /* EL->YSF: one YSFD every 90 ms (identical pacing to DMR->YSF). Runs
     * independently of the EL->DMR pacing above -- both may be active at
     * once in a DMR+YSF+EchoLink bus. */
    if (pairs_with_ysf && core->leg_el_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER
        && bridge_ms_elapsed(&core->leg_el_tx_ysf.last_ysf_tx, YSF_FRAME_MS))
        (void)core_el_emit_ysf_from_conv(core);

    /* End EL->DMR/YSF when inbound EL PCM stops (silence still holds while
     * RTP). Both checked independently -- ending one must not touch the
     * other's phase. */
    if (bridge_ms_since(&core->leg_el_capture.last_el_speech) >= EL_HANG_MS) {
        if (core->leg_el_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER && !core->leg_el_tx_dmr.dmr_ending)
            core_el_dmr_begin_end(core);
        if (core->leg_el_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER && !core->leg_el_tx_ysf.ysf_ending)
            core_el_end_ysf_call(core);
    }

    /* DMR/YSF->EL: if the stream dies without VTERM/EOT, release the
     * half-duplex lock so EL TX can run again. rx_src_kind (stamped when the
     * RX call began) says which timeout/log/reset applies -- a 3-kind bus
     * can have both pairs_with_dmr and pairs_with_ysf true at once, so
     * guessing from that alone (as before Fase 8) picked DMR unconditionally
     * even when YSF was the actual source. */
    if (core->leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER) {
        if (core->leg_el_rx.rx_src_kind == MEDIA_PEER_DMR
            && bridge_ms_since(&core->leg_el_rx.last_dmr_rx) >= DMR_RX_HANG_MS) {
            if (el)
                peer_el_flush_pcm(el);
            LOG_DMR_INFO("%s call end (%d voice frames in, el_rtp_tx=%u) — RX hangtime\n",
                         media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.dmr_voice_frames, el ? el->rtp_tx_packets : 0);
            core_router_release_active(core);
            core->leg_el_rx.phase = MEDIA_CALL_IDLE;
            core->leg_el_rx.dmr_voice_frames = 0;
            core->leg_el_rx.dmr_rx_stream_id = 0;
        } else if (core->leg_el_rx.rx_src_kind == MEDIA_PEER_YSF
                   && bridge_ms_since(&core->leg_el_rx.last_dmr_rx) >= YSF_RX_HANG_MS) {
            if (el) {
                peer_el_flush_pcm(el);
                peer_el_set_talker_name(el, NULL);
            }
            LOG_YSF_INFO("%s call end (%d voice frames in, el_rtp_tx=%u) — RX hangtime\n",
                         media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK),
                         core->leg_el_rx.ysf_voice_frames, el ? el->rtp_tx_packets : 0);
            core_router_release_active(core);
            core->leg_el_rx.phase = MEDIA_CALL_IDLE;
            core->leg_el_rx.ysf_voice_frames = 0;
            modeconv_reset(core->mc_el);
        }
    }
}

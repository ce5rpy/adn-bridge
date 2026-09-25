/*
 * media_core — generic PCM-native peer <-> DMR/YSF bridge (vocoder + ModeConv).
 *
 * Replaces the old media/core_echolink.c: same logic (ported from
 * bridge_el.c, then generalized), but parameterized by media_peer_kind_t so
 * it serves any PCM-native peer kind (EchoLink, ALSA, ...) and any number of
 * instances of each, with all session state living on the peer's own
 * media_peer_slot_t (media/pcm_leg.h) instead of one fixed set of fields on
 * media_core_t. Adding a new PCM-native peer kind needs no new code here —
 * just a couple of lines in pcm_read/pcm_egress/pcm_log_ch/the identity and
 * roster hooks below, and a media_core_ingress dispatch case (media/core.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core_pcm_bridge.h"

#include "adapters/alsa.h"
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

#define DMR_FRAME_MS   60 /* paces DMR TX slightly slower than adapters/dmr.c's 55ms */
#define YSF_FRAME_MS   90
#define CONNECT_PTT_MS 500               /* on-air duration of each connect-PTT burst (4000 and real TG alike) */
#define CONNECT_PTT_START_DELAY_MS 2000  /* silent gap after DMR connects, before the first connect-PTT starts */
#define CONNECT_PTT_GAP_MS         4000  /* silent gap after the 4000 clear-PTT ends, before the real-TG PTT starts */
#define DMR_CLEAR_DYNAMIC_TG 4000
/* End PCM peer -> DMR/YSF after this much without inbound PCM (key-down
 * silence must still hold / activate the TG; hang follows PCM presence). */
#define PCM_HANG_MS 700
/* After PCM peer -> DMR/YSF end, ignore residual PCM (no phantom reopen). */
#define PCM_TX_COOLDOWN_MS 800
/* DMR/YSF -> PCM peer without VTERM/EOT used to leave the leg open forever. */
#define DMR_RX_HANG_MS 1500
#define YSF_RX_HANG_MS 1500

/* =====================================================================
 * The only genuinely per-kind glue: read/write the peer's own PCM, log
 * under its own channel, resolve its own identity, and (EchoLink only)
 * update its inbound roster display. Everything else below is one engine.
 * ===================================================================== */

static int pcm_read(media_peer_kind_t kind, media_peer_slot_t *slot, int16_t *pcm, int max)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        return adapter_el_read_pcm(&slot->u.el, pcm, max);
    if (kind == MEDIA_PEER_ALSA)
        return adapter_alsa_read_pcm(&slot->u.alsa, pcm, max);
    return 0;
}

static int pcm_egress(media_peer_kind_t kind, media_peer_slot_t *slot, const int16_t *pcm, int n)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        return adapter_el_egress_pcm(&slot->u.el, pcm, n);
    if (kind == MEDIA_PEER_ALSA)
        return adapter_alsa_egress_pcm(&slot->u.alsa, pcm, n);
    return 0;
}

static log_channel_t pcm_log_ch(media_peer_kind_t kind)
{
    return kind == MEDIA_PEER_ALSA ? LOG_CH_ALSA : LOG_CH_ECHOLINK;
}

/* How long to wait, at the core level, without a captured chunk before
 * ending a TX_TO_PEER call (see PCM_HANG_MS). EchoLink's peer has no local
 * "should I still be on air" concept -- it just forwards whatever RTP
 * arrives -- so this IS its only hang mechanism (full PCM_HANG_MS). ALSA's
 * own VOX (peer_alsa.c: [peer.*] vox_hang_ms) already fully implements that
 * decision -- read_pcm() stops yielding samples the instant VOX itself ends
 * the call. Applying the full PCM_HANG_MS again on top would silently stack
 * a second, non-configurable hangtime under the user's back (observed in
 * the field as an unexpectedly long/inconsistent tail before the DMR call
 * actually ends). A small margin here just absorbs tick-timing jitter, not
 * a second hang decision -- the user's real, single knob for "how long to
 * keep transmitting through brief silence" is vox_hang_ms. */
static int pcm_capture_hang_ms(media_peer_kind_t kind)
{
    return kind == MEDIA_PEER_ALSA ? 100 : PCM_HANG_MS;
}

/* Drop any staged inbound PCM / reset per-peer capture state on call end. */
static void pcm_drop_in(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        peer_el_drop_pcm_in(&slot->u.el);
    else if (kind == MEDIA_PEER_ALSA)
        peer_alsa_drop_pcm_in(&slot->u.alsa);
}

static void pcm_flush_out(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        peer_el_flush_pcm(&slot->u.el);
    else if (kind == MEDIA_PEER_ALSA)
        peer_alsa_flush_pcm(&slot->u.alsa);
}

/* Roster/display update -- EchoLink-only (inbound "connected users" list and
 * SDES NAME); no-op for ALSA (no roster concept). */
static void pcm_set_relay_label(media_peer_kind_t kind, media_peer_slot_t *slot, const char *label)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        peer_el_set_relay_label(&slot->u.el, label);
}

static void pcm_set_talker_name(media_peer_kind_t kind, media_peer_slot_t *slot, const char *name)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        peer_el_set_talker_name(&slot->u.el, name);
}

static void pcm_clear_remote_talker(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    if (kind == MEDIA_PEER_ECHOLINK)
        peer_el_clear_remote_talker(&slot->u.el);
}

/* Best remote identity for this PCM peer -> DMR/YSF: EchoLink resolves it
 * from the RTP/SDES-negotiated remote talker (falling back to its own
 * callsign). ALSA carries no identity of its own at all -- always "", so
 * the resolve functions below fall through to the DMR/bridge identity
 * fallback chain, attributing the traffic to the bridge itself. */
static const char *pcm_identity_raw(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    if (kind == MEDIA_PEER_ECHOLINK) {
        peer_echolink_t *el = &slot->u.el;
        const char *raw = peer_el_remote_talker(el);

        return (raw && raw[0]) ? raw : el->callsign;
    }
    return "";
}

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

static int pcm_vocoder_active(media_peer_slot_t *slot)
{
    return slot->pcm_voc_ready && vocoder_is_ready(&slot->pcm_voc);
}

static int pcm_to_ambe(media_peer_slot_t *slot, const int16_t *pcm, uint8_t ambe[7])
{
    if (!pcm_vocoder_active(slot))
        return -1;
    return vocoder_encode(&slot->pcm_voc, pcm, ambe);
}

static int ambe_to_pcm(media_peer_slot_t *slot, const uint8_t ambe[7], int16_t pcm[160])
{
    if (!pcm_vocoder_active(slot))
        return -1;
    return vocoder_decode(&slot->pcm_voc, ambe, pcm);
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

static int core_router_take_kind_slot(media_core_t *core, media_peer_slot_t *slot)
{
    return core_router_take(core, slot->router_id);
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

/* Best-known talker id for the currently active PCM peer->DMR TX leg, else
 * the bridge peer's own id, else the bridge_dmrid fallback (valid even with
 * no live DMR peer, e.g. a PCM<->YSF-only layout). */
static int pcm_rf_id_or_bridge(media_core_t *core, media_peer_slot_t *slot, peer_dmr_t *dmr)
{
    if (slot->pcm_tx_dmr.call.talker_id > 0)
        return slot->pcm_tx_dmr.call.talker_id;
    if (dmr && dmr->dmrid > 0)
        return dmr->dmrid;
    return core->bridge_dmrid;
}

/* ---- talker identity ---- */

static void pcm_resolve_talker_for_dmr(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    const char *raw = pcm_identity_raw(kind, slot);
    char base[16];
    char talker10[10];
    int id = 0;
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);

    identity_callsign_base(raw, base);
    identity_format_base_callsign10(talker10, base);
    id = identity_callsign10_to_dmrid((const uint8_t *)talker10);
    if (id <= 0 && base[0] && core->aliases)
        id = identity_lookup_alias_id(core->aliases, base);

    if (id > 0) {
        memcpy(slot->pcm_tx_dmr.call.netcall.net_src, talker10, 10);
        slot->pcm_tx_dmr.call.talker_id = id;
        LOG_DMR_INFO("%s talker %s -> id %d (alias)\n",
                     media_flow_label(kind, MEDIA_PEER_DMR), base, id);
        return;
    }
    if (dmr && dmr->dmrid > 0) {
        memcpy(slot->pcm_tx_dmr.call.netcall.net_src, dmr->callsign, 10);
        slot->pcm_tx_dmr.call.talker_id = dmr->dmrid;
        LOG_DMR_INFO("%s talker %s unknown -> bridge %.10s id %d\n",
                     media_flow_label(kind, MEDIA_PEER_DMR),
                     base[0] ? base : "?", slot->pcm_tx_dmr.call.netcall.net_src,
                     slot->pcm_tx_dmr.call.talker_id);
        return;
    }
    if (core->bridge_dmrid > 0) {
        slot->pcm_tx_dmr.call.talker_id = core->bridge_dmrid;
        LOG_DMR_INFO("%s talker %s unknown -> bridge id %d\n",
                     media_flow_label(kind, MEDIA_PEER_DMR),
                     base[0] ? base : "?", slot->pcm_tx_dmr.call.talker_id);
        return;
    }
    slot->pcm_tx_dmr.call.talker_id = 0;
    LOG_DMR_WARNING("%s talker %s: no DMR id (alias miss, no bridge dmrid)\n",
                    media_flow_label(kind, MEDIA_PEER_DMR),
                    base[0] ? base : "?");
}

static void pcm_resolve_talker_for_ysf(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    const char *raw = pcm_identity_raw(kind, slot);
    char base[16];
    char prev[10];
    int id = 0;

    identity_callsign_base(raw, base);
    memcpy(prev, slot->pcm_tx_ysf.call.netcall.net_src, 10);
    identity_format_full_callsign10(slot->pcm_tx_ysf.call.netcall.net_src, raw);
    {
        char talker10[10];

        identity_format_base_callsign10(talker10, base);
        id = identity_callsign10_to_dmrid((const uint8_t *)talker10);
        if (id <= 0 && base[0] && core->aliases)
            id = identity_lookup_alias_id(core->aliases, base);
    }
    slot->pcm_tx_ysf.call.talker_id = id > 0 ? id : core->bridge_dmrid;
    if (memcmp(prev, slot->pcm_tx_ysf.call.netcall.net_src, 10) != 0)
        LOG_YSF_INFO("%s talker raw=%s base=%s\n",
                     media_flow_label(kind, MEDIA_PEER_YSF), raw, base[0] ? base : "?");
}

/* RX-from-YSF only: who's currently talking INTO this PCM peer (EchoLink
 * roster display only; no-op for ALSA). */
static void pcm_set_ysf_talker_name(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    char talker[16];
    char name[32];
    char label[32];

    if (kind != MEDIA_PEER_ECHOLINK)
        return;
    identity_wire_call_to_cstr(talker, slot->pcm_rx.call.netcall.net_src);
    if (!talker[0]) {
        pcm_set_talker_name(kind, slot, NULL);
        pcm_set_relay_label(kind, slot, "YSF");
        return;
    }
    snprintf(name, sizeof(name), "%.10s (%.12s)", slot->u.el.callsign, talker);
    pcm_set_talker_name(kind, slot, name);
    snprintf(label, sizeof(label), "YSF %.12s", talker);
    pcm_set_relay_label(kind, slot, label);
}

/* =====================================================================
 * PCM peer <-> DMR
 * ===================================================================== */

static void core_reset_dmr_tx_slots(media_core_t *core)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *s = &core->bus->slots[i];

        if (s->kind != MEDIA_PEER_DMR)
            continue;
        s->dmr_tx_seq = 0;
        s->dmr_tx_stream_id = bridge_new_stream_id();
    }
}

static void pcm_dmr_tx_dmrd(media_core_t *core, media_peer_slot_t *slot,
                           media_peer_slot_t *dmr_slot, uint8_t frame_type, const uint8_t *voice33)
{
    dmr_tx_args_t args;
    peer_dmr_t *dmr;

    if (!dmr_slot || !dmr_slot->open)
        return;
    dmr = &dmr_slot->u.dmr;

    if (dmr_slot->cp_active && dmr_slot->cp_clearing)
        frame_type |= DMRD_CALL_PRIVATE;

    args.peer = dmr;
    args.bridge_dmrid = dmr->dmrid;
    args.talker_rf_id = pcm_rf_id_or_bridge(core, slot, dmr);
    args.tx_tg = (dmr_slot->cp_active && dmr_slot->cp_tg > 0) ? dmr_slot->cp_tg : dmr->tg;
    args.seq = &dmr_slot->dmr_tx_seq;
    args.stream_id = dmr_slot->dmr_tx_stream_id;
    args.last_tx = &slot->pcm_tx_dmr.last_tx;
    args.emb_raw = dmr_slot->dmr_emb_raw;
    adapter_dmr_egress_dmrd(&args, frame_type, voice33);
}

typedef struct {
    media_core_t      *core;
    media_peer_slot_t *src_slot;
    uint8_t            frame_type;
    const uint8_t     *voice33;
} pcm_fanout_dmrd_ctx_t;

static int pcm_fanout_dmrd_cb(int dst_id, media_peer_kind_t dst_kind, void *vctx)
{
    pcm_fanout_dmrd_ctx_t *ctx = vctx;
    media_peer_slot_t *dst;

    if (dst_kind != MEDIA_PEER_DMR)
        return 0;
    dst = media_peer_bus_slot_mut(ctx->core->bus, dst_id);
    if (!dst)
        return 0;
    pcm_dmr_tx_dmrd(ctx->core, ctx->src_slot, dst, ctx->frame_type, ctx->voice33);
    return 0;
}

static void pcm_send_dmrd(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot,
                          uint8_t frame_type, const uint8_t *voice33)
{
    pcm_fanout_dmrd_ctx_t ctx = { core, slot, frame_type, voice33 };

    (void)kind;
    if (core->router)
        media_router_fanout(core->router, slot->router_id, pcm_fanout_dmrd_cb, &ctx);
}

static void pcm_dmr_emit_voice(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot,
                               const uint8_t voice33[33])
{
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
    uint8_t slot_bit = core->dmr_slot_bit;
    uint8_t n = (uint8_t)(slot->pcm_tx_dmr.voice_frames % 6);
    uint8_t b15;

    if (slot->pcm_tx_dmr.phase == MEDIA_CALL_IDLE) {
        if (!core_router_take_kind_slot(core, slot))
            return;
        slot->pcm_tx_dmr.phase = MEDIA_CALL_TX_TO_PEER;
        slot->pcm_tx_dmr.call.stream_id = bridge_new_stream_id();
        slot->pcm_tx_dmr.frame_cnt = 0;
        slot->pcm_tx_dmr.voice_frames = 0;
        core_reset_dmr_tx_slots(core);
        modeconv_reset(slot->pcm_mc_dmr);
        pcm_resolve_talker_for_dmr(core, kind, slot);
        /* One VHEAD only: identical repeats are counted as loss (dup CRC /
         * lastData) by adn-server PacketControl and create SEQ gaps. */
        pcm_send_dmrd(core, kind, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                      NULL);
        if (kind == MEDIA_PEER_ECHOLINK)
            slot->u.el.rtp_rx_packets = 0;
        LOG_DMR_INFO("%s call start (TG %d, src %.10s id %d)\n",
                     media_flow_label(kind, MEDIA_PEER_DMR),
                     dmr ? dmr->tg : 0, slot->pcm_tx_dmr.call.netcall.net_src,
                     slot->pcm_tx_dmr.call.talker_id);
    }

    b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
    pcm_send_dmrd(core, kind, slot, b15, voice33);
    slot->pcm_tx_dmr.voice_frames++;
}

/* Begin paced teardown: pad to superframe then VTERM at DMR_FRAME_MS. */
static void pcm_dmr_begin_end(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    if (slot->pcm_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER || slot->pcm_tx_dmr.ending)
        return;
    slot->pcm_tx_dmr.ending = 1;
    slot->pcm_tx_dmr.ambe_count = 0;
    slot->pcm_capture.pcm_acc_n = 0;
    pcm_drop_in(kind, slot);
    modeconv_reset(slot->pcm_mc_dmr);
    /* Allow first pad/VTERM on the next tick immediately. */
    slot->pcm_tx_dmr.last_tx.tv_sec = 0;
    slot->pcm_tx_dmr.last_tx.tv_nsec = 0;
}

static void pcm_dmr_finish_end(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    unsigned rtp_rx = (kind == MEDIA_PEER_ECHOLINK) ? slot->u.el.rtp_rx_packets : 0;

    LOG_DMR_INFO("%s call end (%d DMR frames out, rx=%u, seq=%u)\n",
                 media_flow_label(kind, MEDIA_PEER_DMR),
                 slot->pcm_tx_dmr.voice_frames, rtp_rx, (unsigned)slot->pcm_tx_dmr.frame_cnt);
    core_router_release_active(core);
    slot->pcm_tx_dmr.phase = MEDIA_CALL_IDLE;
    slot->pcm_tx_dmr.ending = 0;
    slot->pcm_tx_dmr.voice_frames = 0;
    slot->pcm_tx_dmr.ambe_count = 0;
    slot->pcm_tx_dmr.speech_run = 0;
    slot->pcm_tx_dmr.call.talker_id = 0;
    pcm_drop_in(kind, slot);
    pcm_clear_remote_talker(kind, slot);
    modeconv_reset(slot->pcm_mc_dmr);
    bridge_stamp_now(&slot->pcm_tx_dmr.last_tx_end);
}

static void pcm_dmr_pace_end(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    if (!slot->pcm_tx_dmr.ending || slot->pcm_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER)
        return;
    if (!bridge_ms_elapsed(&slot->pcm_tx_dmr.last_tx, DMR_FRAME_MS))
        return;

    if ((slot->pcm_tx_dmr.voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(slot->pcm_tx_dmr.voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4))
                                       : (slot_bit | n));
        pcm_send_dmrd(core, kind, slot, b15, DMR_SILENCE_DATA);
        slot->pcm_tx_dmr.voice_frames++;
        return;
    }
    pcm_send_dmrd(core, kind, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                  DMR_SILENCE_DATA);
    pcm_dmr_finish_end(core, kind, slot);
}

/* Immediate VTERM (no pad burst) — used when DMR RX preempts PCM peer TX. */
static void pcm_dmr_end_call(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    if (slot->pcm_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER && !slot->pcm_tx_dmr.ending)
        return;
    if (slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
        pcm_send_dmrd(core, kind, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                      DMR_SILENCE_DATA);
    pcm_dmr_finish_end(core, kind, slot);
}

/* Accumulate one AMBE frame toward the next DMR voice33 (group of 3). */
static void pcm_dmr_feed_ambe(media_peer_kind_t kind, media_peer_slot_t *slot, int rms, const uint8_t ambe[7])
{
    int in_cooldown = (slot->pcm_tx_dmr.last_tx_end.tv_sec || slot->pcm_tx_dmr.last_tx_end.tv_nsec)
                      && bridge_ms_since(&slot->pcm_tx_dmr.last_tx_end) < PCM_TX_COOLDOWN_MS;

    if (slot->pcm_tx_dmr.phase == MEDIA_CALL_IDLE) {
        if (in_cooldown) {
            static int drop_dbg;
            if (++drop_dbg <= 3 || (drop_dbg % 50) == 0)
                LOG_CH_DEBUG(pcm_log_ch(kind), "post-TX cooldown drop rms=%d\n", rms);
            slot->pcm_tx_dmr.ambe_count = 0;
            slot->pcm_tx_dmr.speech_run = 0;
            return;
        }
        if (!slot->pcm_tx_dmr.speech_run) {
            LOG_CH_INFO(pcm_log_ch(kind), "audio rms=%d — starting %s path (TG activate)\n",
                       rms, media_flow_label(kind, MEDIA_PEER_DMR));
            slot->pcm_tx_dmr.speech_run = 1;
        }
    }

    memcpy(slot->pcm_tx_dmr.ambe_buf[slot->pcm_tx_dmr.ambe_count], ambe, 7);
    slot->pcm_tx_dmr.ambe_count++;
    if (slot->pcm_tx_dmr.ambe_count < 3)
        return;

    modeconv_put_ambe7(slot->pcm_mc_dmr, slot->pcm_tx_dmr.ambe_buf[0]);
    modeconv_put_ambe7(slot->pcm_mc_dmr, slot->pcm_tx_dmr.ambe_buf[1]);
    modeconv_put_ambe7(slot->pcm_mc_dmr, slot->pcm_tx_dmr.ambe_buf[2]);
    slot->pcm_tx_dmr.ambe_count = 0;
}

/* forward decls for cross-references between the DMR and YSF sections */
static void pcm_end_ysf_call(media_peer_kind_t kind, media_peer_slot_t *slot);

static void pcm_ingress_dmr_one(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot,
                                int src_router_id, const media_bus_frame_t *frame)
{
    uint8_t ambe[3][7];
    int16_t pcm[160];
    int i;

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN: {
        uint32_t sid = frame->meta.stream_id;

        if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER && sid == slot->pcm_rx.rx_stream_id) {
            bridge_stamp_now(&slot->pcm_rx.last_rx);
            return;
        }
        if (slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
            pcm_dmr_end_call(core, kind, slot);
        if (slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
            pcm_end_ysf_call(kind, slot);
        if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER) {
            pcm_flush_out(kind, slot);
            pcm_set_relay_label(kind, slot, NULL);
            LOG_DMR_INFO("%s call end (%d voice frames in) — replaced by new stream\n",
                         media_flow_label(MEDIA_PEER_DMR, kind), slot->pcm_rx.dmr_voice_frames);
            core_router_release_active(core);
        }
        if (!core_router_take(core, src_router_id))
            return;
        slot->pcm_rx.phase = MEDIA_CALL_RX_FROM_PEER;
        slot->pcm_rx.rx_src_kind = MEDIA_PEER_DMR;
        slot->pcm_rx.rx_stream_id = sid;
        slot->pcm_rx.dmr_voice_frames = 0;
        if (kind == MEDIA_PEER_ECHOLINK) {
            char cs[16];
            char label[32];

            slot->u.el.rtp_tx_packets = 0;
            identity_dmr_display_callsign(core->aliases, frame->meta.talker_id, cs);
            snprintf(label, sizeof(label), cs[0] ? "DMR %s" : "DMR", cs);
            pcm_set_relay_label(kind, slot, label);
        }
        bridge_stamp_now(&slot->pcm_rx.last_rx);
        LOG_DMR_INFO("%s call start\n", media_flow_label(MEDIA_PEER_DMR, kind));
        return;
    }
    case MEDIA_FRAME_CALL_END:
        if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER && slot->pcm_rx.rx_src_kind == MEDIA_PEER_DMR) {
            pcm_flush_out(kind, slot);
            pcm_set_relay_label(kind, slot, NULL);
            LOG_DMR_INFO("%s call end (%d voice frames in)\n",
                         media_flow_label(MEDIA_PEER_DMR, kind), slot->pcm_rx.dmr_voice_frames);
            core_router_release_active(core);
            slot->pcm_rx.phase = MEDIA_CALL_IDLE;
            slot->pcm_rx.dmr_voice_frames = 0;
            slot->pcm_rx.rx_stream_id = 0;
        }
        return;
    case MEDIA_FRAME_VOICE:
        if (slot->pcm_rx.phase != MEDIA_CALL_RX_FROM_PEER || slot->pcm_rx.rx_src_kind != MEDIA_PEER_DMR)
            return;
        if (!pcm_vocoder_active(slot))
            return;
        (void)src_router_id;
        bridge_stamp_now(&slot->pcm_rx.last_rx);
        slot->pcm_rx.dmr_voice_frames++;
        modeconv_dmr33_to_ambe(frame->payload.dmr_voice33, ambe);
        for (i = 0; i < 3; i++) {
            if (ambe_to_pcm(slot, ambe[i], pcm) != 0) {
                static int voc_fail;
                if (bridge_dbg_periodic(&voc_fail))
                    LOG_DMR_WARNING("%s vocoder decode failed\n", media_flow_label(MEDIA_PEER_DMR, kind));
                continue;
            }
            /* Half-duplex ALSA-only: don't feed RX audio to the same box's
             * speaker while its own mic is currently transmitting (avoid
             * acoustic feedback). EchoLink has no such coupling. */
            if (kind == MEDIA_PEER_ALSA && slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
                continue;
            pcm_egress(kind, slot, pcm, 160);
        }
        return;
    default:
        return;
    }
}

void core_pcm_bridge_ingress_dmr(media_core_t *core, media_peer_kind_t pcm_kind,
                                  int src_router_id, const media_bus_frame_t *frame)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (slot->open && slot->kind == pcm_kind)
            pcm_ingress_dmr_one(core, pcm_kind, slot, src_router_id, frame);
    }
}

/* ---- connect-PTT (DMR login rising edge) — per DMR destination slot.
 * Generic across every PCM-native peer kind: gated on whether ANYONE
 * currently holds the router's ingress lock (media_router_active_ingress),
 * not on one hardcoded peer's own tx leg -- so it correctly waits regardless
 * of which PCM peer (or how many) might be mid-call. */

static void core_pcm_cp_begin_stream(media_peer_slot_t *slot, int tg, int clearing)
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

static void core_pcm_cp_begin_wait(media_peer_slot_t *slot, int phase, int next_tg, int next_clearing)
{
    slot->cp_active = 1;
    slot->cp_phase = phase;
    slot->cp_voice_frames = 0;
    slot->cp_tg = next_tg;
    slot->cp_clearing = next_clearing ? 1 : 0;
    bridge_stamp_now(&slot->cp_start);
}

/* Generic connect-PTT frame sender -- not tied to any particular PCM peer's
 * identity (a synthetic priming burst has no real talker). */
static void core_pcm_cp_tx_dmrd(media_core_t *core, media_peer_slot_t *dmr_slot,
                                uint8_t frame_type, const uint8_t *voice33)
{
    dmr_tx_args_t args;
    peer_dmr_t *dmr;

    if (!dmr_slot || !dmr_slot->open)
        return;
    dmr = &dmr_slot->u.dmr;
    if (dmr_slot->cp_active && dmr_slot->cp_clearing)
        frame_type |= DMRD_CALL_PRIVATE;

    args.peer = dmr;
    args.bridge_dmrid = dmr->dmrid;
    /* No PCM-peer call is active during connect-PTT (a synthetic priming
     * burst, not tied to any specific peer) -- mirror the non-active-call
     * fallback order: this DMR peer's own id, else the bridge fallback. */
    args.talker_rf_id = dmr->dmrid > 0 ? dmr->dmrid : core->bridge_dmrid;
    args.tx_tg = (dmr_slot->cp_active && dmr_slot->cp_tg > 0) ? dmr_slot->cp_tg : dmr->tg;
    args.seq = &dmr_slot->dmr_tx_seq;
    args.stream_id = dmr_slot->dmr_tx_stream_id;
    args.last_tx = &dmr_slot->cp_last_tx; /* restamped by dmr_tx_send() on every real send --
                                            * this is the per-frame pacing clock the poll loop
                                            * checks (media/peer_bus.h: cp_last_tx vs cp_start). */
    args.emb_raw = dmr_slot->dmr_emb_raw;
    adapter_dmr_egress_dmrd(&args, frame_type, voice33);
}

static void core_pcm_cp_finish(media_core_t *core, media_peer_slot_t *slot)
{
    uint8_t slot_bit = core->dmr_slot_bit;
    peer_dmr_t *dmr = &slot->u.dmr;
    int ended_tg = slot->cp_tg > 0 ? slot->cp_tg : dmr->tg;
    int was_clearing = slot->cp_clearing;

    while ((slot->cp_voice_frames % 6) != 0) {
        uint8_t n = (uint8_t)(slot->cp_voice_frames % 6);
        core_pcm_cp_tx_dmrd(core, slot, (uint8_t)(slot_bit | n), DMR_SILENCE_DATA);
        slot->cp_voice_frames++;
    }
    core_pcm_cp_tx_dmrd(core, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VTERM),
                       DMR_SILENCE_DATA);
    LOG_DMR_INFO("DMR connect PTT end (TG %d, %d voice frames) [%.10s]\n",
                 ended_tg, slot->cp_voice_frames, dmr->callsign);

    if (was_clearing && dmr->tg > 0) {
        core_pcm_cp_begin_wait(slot, 2, dmr->tg, 0);
        return;
    }
    slot->cp_active = 0;
    slot->cp_phase = 0;
    slot->cp_voice_frames = 0;
    slot->cp_tg = 0;
    slot->cp_clearing = 0;
}

/* Silently abort every enabled PCM peer's in-progress TX to DMR — called
 * whenever ANY DMR peer disconnects (matches the original bridge_el.c
 * behavior: it doesn't try to figure out whether the specific dropped DMR
 * peer was this call's actual destination). */
static void pcm_abort_to_dmr_all(media_core_t *core, media_peer_kind_t kind)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (!slot->open || slot->kind != kind)
            continue;
        if (slot->pcm_tx_dmr.phase != MEDIA_CALL_TX_TO_PEER && !slot->pcm_tx_dmr.ending)
            continue;
        LOG_DMR_INFO("%s aborted — DMR peer down (was phase=%d ending=%d)\n",
                     media_flow_label(kind, MEDIA_PEER_DMR),
                     slot->pcm_tx_dmr.phase, slot->pcm_tx_dmr.ending);
        core_router_release_active(core);
        slot->pcm_tx_dmr.phase = MEDIA_CALL_IDLE;
        slot->pcm_tx_dmr.ending = 0;
        slot->pcm_tx_dmr.voice_frames = 0;
        slot->pcm_tx_dmr.ambe_count = 0;
        slot->pcm_tx_dmr.speech_run = 0;
        slot->pcm_tx_dmr.call.talker_id = 0;
        pcm_drop_in(kind, slot);
        pcm_clear_remote_talker(kind, slot);
        modeconv_reset(slot->pcm_mc_dmr);
        bridge_stamp_now(&slot->pcm_tx_dmr.last_tx_end);
    }
}

static void core_pcm_start_connect_ptt(media_core_t *core, media_peer_slot_t *slot)
{
    peer_dmr_t *dmr = &slot->u.dmr;

    if (media_router_active_ingress(core->router) >= 0 || slot->cp_active)
        return;
    if (dmr->tg <= 0)
        return;
    if (slot->clear_dynamic_tg)
        core_pcm_cp_begin_wait(slot, 3, DMR_CLEAR_DYNAMIC_TG, 1);
    else
        core_pcm_cp_begin_wait(slot, 3, dmr->tg, 0);
}

static void core_pcm_emit_connect_ptt(media_core_t *core, media_peer_slot_t *slot)
{
    uint8_t slot_bit = core->dmr_slot_bit;

    if (!slot->cp_active)
        return;

    if (slot->cp_phase == 3) {
        if (bridge_ms_since(&slot->cp_start) >= CONNECT_PTT_START_DELAY_MS)
            core_pcm_cp_begin_stream(slot, slot->cp_tg, slot->cp_clearing);
        return;
    }
    if (slot->cp_phase == 2) {
        if (bridge_ms_since(&slot->cp_start) >= CONNECT_PTT_GAP_MS)
            core_pcm_cp_begin_stream(slot, slot->cp_tg, slot->cp_clearing);
        return;
    }
    if (slot->cp_phase == 0) {
        core_pcm_cp_tx_dmrd(core, slot, (uint8_t)(slot_bit | (DMRD_FT_DATA_SYNC << 4) | DMRD_DTYPE_VHEAD),
                           NULL);
        slot->cp_phase = 1;
        bridge_stamp_now(&slot->cp_start);
        return;
    }
    if (bridge_ms_since(&slot->cp_start) >= CONNECT_PTT_MS) {
        core_pcm_cp_finish(core, slot);
        return;
    }
    {
        uint8_t n = (uint8_t)(slot->cp_voice_frames % 6);
        uint8_t b15 = (uint8_t)(n == 0 ? (slot_bit | (DMRD_FT_VOICE_SYNC << 4)) : (slot_bit | n));
        core_pcm_cp_tx_dmrd(core, slot, b15, DMR_SILENCE_DATA);
        slot->cp_voice_frames++;
    }
}

static void core_pcm_dmr_poll_connect_ptt(media_core_t *core, media_peer_kind_t kind)
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
            core_pcm_start_connect_ptt(core, slot);
        if (!connected) {
            slot->cp_active = 0;
            slot->cp_phase = 0;
            slot->cp_voice_frames = 0;
            slot->cp_tg = 0;
            slot->cp_clearing = 0;
            pcm_abort_to_dmr_all(core, kind);
        }
        slot->dmr_was_connected = connected;

        if (slot->cp_active && bridge_ms_elapsed(&slot->cp_last_tx, DMR_FRAME_MS))
            core_pcm_emit_connect_ptt(core, slot);
    }
}

static int core_pcm_any_connect_ptt_active(const media_core_t *core)
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

static void pcm_dmr_pace_tx(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    uint8_t voice33[33];

    if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER || core_pcm_any_connect_ptt_active(core)
        || slot->pcm_tx_dmr.ending)
        return;
    if (slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER
        && !bridge_ms_elapsed(&slot->pcm_tx_dmr.last_tx, DMR_FRAME_MS))
        return;
    if (modeconv_get_dmr(slot->pcm_mc_dmr, voice33) == MODECONV_TAG_DATA)
        pcm_dmr_emit_voice(core, kind, slot, voice33);
}

/* =====================================================================
 * PCM peer <-> YSF
 * ===================================================================== */

static int pcm_tx_ysfd(media_peer_slot_t *slot, peer_ysf_t *ysf, uint8_t fi, uint8_t ft,
                       uint8_t cm, uint8_t fich_fn, uint8_t net_cnt,
                       const uint8_t *payload120, const uint8_t csd1[20], const uint8_t csd2[20])
{
    ysf_tx_args_t args;

    if (!ysf)
        return 0;
    args = (ysf_tx_args_t){
        .peer = ysf,
        .repeater_callsign = ysf->callsign,
        .meta = &slot->pcm_tx_ysf.call.netcall,
        .last_tx = &slot->pcm_tx_ysf.last_tx,
        .dgid_cfg = ysf->dgid,
    };
    return adapter_ysf_egress_ysfd(&args, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2);
}

/* Fan out to every enabled YSF peer except the source PCM peer's own slot
 * (iterating the bus directly rather than media_router_fanout -- no callback
 * context needed since the payload args are already fully resolved here). */
static void pcm_send_ysfd(media_core_t *core, media_peer_slot_t *slot, uint8_t fi, uint8_t ft, uint8_t cm,
                          uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                          const uint8_t csd1[20], const uint8_t csd2[20])
{
    int i;

    if (!core->router || !core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *dst = &core->bus->slots[i];

        if (dst->kind != MEDIA_PEER_YSF || !dst->open || dst->router_id == slot->router_id)
            continue;
        pcm_tx_ysfd(slot, &dst->u.ysf, fi, ft, cm, fich_fn, net_cnt, payload120, csd1, csd2);
    }
}

static int pcm_emit_ysf_from_conv(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    uint8_t payload[120];
    unsigned int tag;

    memset(payload, 0, sizeof(payload));
    tag = modeconv_get_ysf(slot->pcm_mc_ysf, payload);
    if (tag == MODECONV_TAG_NODATA)
        return 0;

    if (tag == MODECONV_TAG_HEADER) {
        uint8_t csd1[20], csd2[20];

        slot->pcm_tx_ysf.frame_cnt = 0;
        ysf_tx_fill_csd(&slot->pcm_tx_ysf.call.netcall, csd1, csd2);
        pcm_send_ysfd(core, slot, YSF_FI_HEADER, YSF_FICH_FT, YSF_FICH_CM, 0, 0, NULL, csd1, csd2);
        slot->pcm_tx_ysf.frame_cnt = 1;
        return 1;
    }
    if (tag == MODECONV_TAG_EOT) {
        uint8_t csd1[20], csd2[20];

        ysf_tx_fill_csd(&slot->pcm_tx_ysf.call.netcall, csd1, csd2);
        pcm_send_ysfd(core, slot, YSF_FI_TERMINATOR, YSF_FICH_FT, YSF_FICH_CM, 0,
                      slot->pcm_tx_ysf.frame_cnt, NULL, csd1, csd2);
        LOG_YSF_INFO("%s call end (%d voice frames out)\n",
                     media_flow_label(kind, MEDIA_PEER_YSF), slot->pcm_tx_ysf.voice_frames);
        core_router_release_active(core);
        slot->pcm_tx_ysf.phase = MEDIA_CALL_IDLE;
        slot->pcm_tx_ysf.ending = 0;
        slot->pcm_tx_ysf.voice_frames = 0;
        slot->pcm_tx_ysf.ambe_count = 0;
        slot->pcm_tx_ysf.frame_cnt = 0;
        slot->pcm_tx_ysf.speech_run = 0;
        slot->pcm_tx_ysf.call.talker_id = 0;
        pcm_drop_in(kind, slot);
        pcm_clear_remote_talker(kind, slot);
        modeconv_reset(slot->pcm_mc_ysf);
        bridge_stamp_now(&slot->pcm_tx_ysf.last_tx_end);
        return 1;
    }
    if (tag == MODECONV_TAG_DATA) {
        uint8_t fn = (uint8_t)((slot->pcm_tx_ysf.frame_cnt - 1U) % (YSF_FICH_FT + 1U));
        uint8_t net = (uint8_t)((slot->pcm_tx_ysf.frame_cnt & 0x7FU) << 1);

        pcm_send_ysfd(core, slot, YSF_FI_COMMUNICATIONS, YSF_FICH_FT, YSF_FICH_CM, fn, net,
                      payload, NULL, NULL);
        slot->pcm_tx_ysf.voice_frames++;
        slot->pcm_tx_ysf.frame_cnt++;
        return 1;
    }
    return 0;
}

static void pcm_end_ysf_call(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    (void)kind;
    if (slot->pcm_tx_ysf.phase != MEDIA_CALL_TX_TO_PEER || slot->pcm_tx_ysf.ending)
        return;
    slot->pcm_tx_ysf.ending = 1;
    modeconv_put_dmr_eot(slot->pcm_mc_ysf);
}

static void pcm_begin_to_ysf(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    pcm_resolve_talker_for_ysf(core, kind, slot);
    memset(slot->pcm_tx_ysf.call.netcall.net_dst, ' ', 10);
    memcpy(slot->pcm_tx_ysf.call.netcall.net_dst, YSF_WIRE_DST_ALL, 10);
    if (!core_router_take_kind_slot(core, slot))
        return;
    slot->pcm_tx_ysf.phase = MEDIA_CALL_TX_TO_PEER;
    slot->pcm_tx_ysf.ending = 0;
    slot->pcm_tx_ysf.voice_frames = 0;
    slot->pcm_tx_ysf.ambe_count = 0;
    slot->pcm_tx_ysf.frame_cnt = 0;
    modeconv_reset(slot->pcm_mc_ysf);
    modeconv_put_dmr_header(slot->pcm_mc_ysf);
    if (kind == MEDIA_PEER_ECHOLINK)
        slot->u.el.rtp_rx_packets = 0;
    LOG_YSF_INFO("%s call start (src %.10s)\n",
                 media_flow_label(kind, MEDIA_PEER_YSF), slot->pcm_tx_ysf.call.netcall.net_src);
}

/* Late SDES user talker: radios lock HEADER — re-queue HEADER with new CSD.
 * EchoLink-only (ALSA's identity is static, never changes mid-call). */
static void pcm_ysf_reheader_if_talker_changed(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    const char *raw;
    char want[10];
    char prev[10];

    if (kind != MEDIA_PEER_ECHOLINK)
        return;
    if (slot->pcm_tx_ysf.phase != MEDIA_CALL_TX_TO_PEER || slot->pcm_tx_ysf.ending)
        return;
    raw = peer_el_remote_talker(&slot->u.el);
    if (!raw || !raw[0])
        return;
    identity_format_full_callsign10(want, raw);
    if (memcmp(want, slot->pcm_tx_ysf.call.netcall.net_src, 10) == 0)
        return;
    memcpy(prev, slot->pcm_tx_ysf.call.netcall.net_src, 10);
    pcm_resolve_talker_for_ysf(core, kind, slot);
    if (memcmp(prev, slot->pcm_tx_ysf.call.netcall.net_src, 10) == 0)
        return;
    modeconv_put_dmr_header(slot->pcm_mc_ysf);
    LOG_YSF_INFO("%s re-HEADER talker %.10s -> %.10s\n",
                 media_flow_label(kind, MEDIA_PEER_YSF), prev, slot->pcm_tx_ysf.call.netcall.net_src);
}

static void pcm_drain_ysf_to_pcm(media_peer_kind_t kind, media_peer_slot_t *slot)
{
    uint8_t voice33[33];
    uint8_t ambe[3][7];
    int16_t pcm[160];
    unsigned int tag;
    int i;

    while ((tag = modeconv_get_dmr(slot->pcm_mc_ysf, voice33)) != MODECONV_TAG_NODATA) {
        if (tag != MODECONV_TAG_DATA)
            continue;
        modeconv_dmr33_to_ambe(voice33, ambe);
        for (i = 0; i < 3; i++) {
            if (ambe_to_pcm(slot, ambe[i], pcm) != 0) {
                static int voc_fail;
                if (bridge_dbg_periodic(&voc_fail))
                    LOG_YSF_WARNING("%s vocoder decode failed\n", media_flow_label(MEDIA_PEER_YSF, kind));
                continue;
            }
            if (kind == MEDIA_PEER_ALSA && slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
                continue; /* half-duplex: don't play RX while this box's mic is TXing */
            pcm_egress(kind, slot, pcm, 160);
        }
    }
}

static void pcm_ysf_feed_ambe(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot,
                              int rms, const uint8_t ambe[7])
{
    int i;
    int in_cooldown = (slot->pcm_tx_ysf.last_tx_end.tv_sec || slot->pcm_tx_ysf.last_tx_end.tv_nsec)
                      && bridge_ms_since(&slot->pcm_tx_ysf.last_tx_end) < PCM_TX_COOLDOWN_MS;

    if (slot->pcm_tx_ysf.phase == MEDIA_CALL_IDLE) {
        if (in_cooldown) {
            static int drop_dbg;
            if (++drop_dbg <= 3 || (drop_dbg % 50) == 0)
                LOG_CH_DEBUG(pcm_log_ch(kind), "post-TX cooldown drop rms=%d (YSF)\n", rms);
            slot->pcm_tx_ysf.ambe_count = 0;
            slot->pcm_tx_ysf.speech_run = 0;
            return;
        }
        if (!slot->pcm_tx_ysf.speech_run) {
            LOG_CH_INFO(pcm_log_ch(kind), "audio rms=%d — starting %s path\n", rms,
                       media_flow_label(kind, MEDIA_PEER_YSF));
            slot->pcm_tx_ysf.speech_run = 1;
        }
        pcm_begin_to_ysf(core, kind, slot);
    }

    memcpy(slot->pcm_tx_ysf.ambe_buf[slot->pcm_tx_ysf.ambe_count], ambe, 7);
    slot->pcm_tx_ysf.ambe_count++;
    if (slot->pcm_tx_ysf.ambe_count < 5)
        return;

    for (i = 0; i < 5; i++)
        modeconv_put_ambe7_ysf(slot->pcm_mc_ysf, slot->pcm_tx_ysf.ambe_buf[i]);
    slot->pcm_tx_ysf.ambe_count = 0;
}

static void pcm_ingress_ysf_one(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot,
                                int src_router_id, const media_bus_frame_t *frame)
{
    uint8_t scratch120[120];

    switch (frame->kind) {
    case MEDIA_FRAME_CALL_BEGIN:
        if (slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
            pcm_end_ysf_call(kind, slot);
        if (slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
            pcm_dmr_end_call(core, kind, slot);
        if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER) {
            pcm_flush_out(kind, slot);
            pcm_set_talker_name(kind, slot, NULL);
            pcm_set_relay_label(kind, slot, NULL);
            LOG_YSF_INFO("%s call end (%d voice frames in) — replaced by new stream\n",
                         media_flow_label(MEDIA_PEER_YSF, kind), slot->pcm_rx.ysf_voice_frames);
            core_router_release_active(core);
        }
        slot->pcm_rx.call.netcall = frame->meta.netcall;
        memset(slot->pcm_rx.call.netcall.net_dst, ' ', 10);
        memcpy(slot->pcm_rx.call.netcall.net_dst, YSF_WIRE_DST_ALL, 10);
        if (!core_router_take(core, src_router_id))
            return;
        slot->pcm_rx.phase = MEDIA_CALL_RX_FROM_PEER;
        slot->pcm_rx.rx_src_kind = MEDIA_PEER_YSF;
        slot->pcm_rx.ysf_voice_frames = 0;
        if (kind == MEDIA_PEER_ECHOLINK)
            slot->u.el.rtp_tx_packets = 0;
        bridge_stamp_now(&slot->pcm_rx.last_rx);
        modeconv_reset(slot->pcm_mc_ysf);
        modeconv_put_ysf_header(slot->pcm_mc_ysf);
        pcm_set_ysf_talker_name(kind, slot);
        LOG_YSF_INFO("%s call start (src %.10s)\n",
                     media_flow_label(MEDIA_PEER_YSF, kind), slot->pcm_rx.call.netcall.net_src);
        return;
    case MEDIA_FRAME_CALL_END:
        if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER && slot->pcm_rx.rx_src_kind == MEDIA_PEER_YSF) {
            modeconv_put_ysf_eot(slot->pcm_mc_ysf);
            pcm_drain_ysf_to_pcm(kind, slot);
            pcm_flush_out(kind, slot);
            pcm_set_talker_name(kind, slot, NULL);
            pcm_set_relay_label(kind, slot, NULL);
            LOG_YSF_INFO("%s call end (%d voice frames in)\n",
                         media_flow_label(MEDIA_PEER_YSF, kind), slot->pcm_rx.ysf_voice_frames);
            core_router_release_active(core);
            slot->pcm_rx.phase = MEDIA_CALL_IDLE;
            slot->pcm_rx.ysf_voice_frames = 0;
            modeconv_reset(slot->pcm_mc_ysf);
        }
        return;
    case MEDIA_FRAME_VOICE:
        if (slot->pcm_rx.phase != MEDIA_CALL_RX_FROM_PEER || slot->pcm_rx.rx_src_kind != MEDIA_PEER_YSF) {
            if (slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
                pcm_end_ysf_call(kind, slot);
            if (slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER)
                pcm_dmr_end_call(core, kind, slot);
            slot->pcm_rx.call.netcall = frame->meta.netcall;
            if (!core_router_take(core, src_router_id))
                return;
            slot->pcm_rx.phase = MEDIA_CALL_RX_FROM_PEER;
            slot->pcm_rx.rx_src_kind = MEDIA_PEER_YSF;
            slot->pcm_rx.ysf_voice_frames = 0;
            if (kind == MEDIA_PEER_ECHOLINK)
                slot->u.el.rtp_tx_packets = 0;
            modeconv_reset(slot->pcm_mc_ysf);
            modeconv_put_ysf_header(slot->pcm_mc_ysf);
            pcm_set_ysf_talker_name(kind, slot);
            LOG_YSF_INFO("%s call start (src %.10s, no HEADER)\n",
                         media_flow_label(MEDIA_PEER_YSF, kind), slot->pcm_rx.call.netcall.net_src);
        }
        bridge_stamp_now(&slot->pcm_rx.last_rx);
        memcpy(scratch120, frame->payload.ysf_payload120, 120);
        modeconv_put_ysf_payload(slot->pcm_mc_ysf, scratch120);
        slot->pcm_rx.ysf_voice_frames++;
        pcm_drain_ysf_to_pcm(kind, slot);
        return;
    default:
        return;
    }
}

void core_pcm_bridge_ingress_ysf(media_core_t *core, media_peer_kind_t pcm_kind,
                                  int src_router_id, const media_bus_frame_t *frame)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (slot->open && slot->kind == pcm_kind)
            pcm_ingress_ysf_one(core, pcm_kind, slot, src_router_id, frame);
    }
}

/* =====================================================================
 * Mic capture -- one vocoder-encode per 160-sample chunk, fanned to
 * whichever of {DMR TX, YSF TX} pathways are currently eligible for THIS
 * slot. Independent per slot -- 2+ PCM peers of the same or different kinds
 * never share an accumulator.
 * ===================================================================== */

static void pcm_process_audio_one(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    peer_dmr_t *dmr = media_peer_bus_primary_dmr(core->bus);
    peer_ysf_t *ysf = media_peer_bus_primary_ysf(core->bus);
    int16_t pcm[160];
    uint8_t ambe[7];
    int n;
    int enc_fail_streak = 0;
    int dmr_ok, ysf_ok;

    if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER)
        return; /* network source has the slot (half-duplex) */
    if (core_pcm_any_connect_ptt_active(core))
        return;

    if (ysf && slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER)
        pcm_ysf_reheader_if_talker_changed(core, kind, slot);

    dmr_ok = dmr && peer_dmr_connected(dmr) && !slot->pcm_tx_dmr.ending;
    ysf_ok = ysf != NULL && !slot->pcm_tx_ysf.ending;
    if (!dmr_ok && !ysf_ok)
        return;

    while ((n = pcm_read(kind, slot, pcm, 160)) > 0) {
        int i;
        int rms;

        for (i = 0; i < n && slot->pcm_capture.pcm_acc_n < 160; i++)
            slot->pcm_capture.pcm_acc[slot->pcm_capture.pcm_acc_n++] = pcm[i];
        if (slot->pcm_capture.pcm_acc_n < 160)
            continue;

        rms = pcm_rms16(slot->pcm_capture.pcm_acc, 160);
        bridge_stamp_now(&slot->pcm_capture.last_speech);

        pcm_apply_gain(slot->pcm_capture.pcm_acc, 160, slot->pcm_gain);
        if (pcm_to_ambe(slot, slot->pcm_capture.pcm_acc, ambe) != 0) {
            static int voc_enc_fail;

            slot->pcm_capture.pcm_acc_n = 0;
            enc_fail_streak++;
            if (bridge_dbg_periodic(&voc_enc_fail))
                LOG_CH_WARNING(pcm_log_ch(kind), "vocoder encode failed\n");
            if (enc_fail_streak >= 3) {
                while (pcm_read(kind, slot, pcm, 160) > 0)
                    ;
                slot->pcm_capture.pcm_acc_n = 0;
                slot->pcm_tx_dmr.ambe_count = 0;
                slot->pcm_tx_ysf.ambe_count = 0;
                break;
            }
            continue;
        }
        enc_fail_streak = 0;
        slot->pcm_capture.pcm_acc_n = 0;

        if (dmr_ok)
            pcm_dmr_feed_ambe(kind, slot, rms, ambe);
        if (ysf_ok)
            pcm_ysf_feed_ambe(core, kind, slot, rms, ambe);
    }
}

void core_pcm_bridge_poll(media_core_t *core, media_peer_kind_t pcm_kind)
{
    int i;

    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (slot->open && slot->kind == pcm_kind)
            pcm_process_audio_one(core, pcm_kind, slot);
    }
}

/* =====================================================================
 * Tick: pace/hang for every enabled slot of pcm_kind.
 * ===================================================================== */

static void pcm_tick_one(media_core_t *core, media_peer_kind_t kind, media_peer_slot_t *slot)
{
    peer_dmr_t *dmr;
    int pairs_with_dmr = core->router && media_router_find_first(core->router, MEDIA_PEER_DMR) >= 0;
    int pairs_with_ysf = core->router && media_router_find_first(core->router, MEDIA_PEER_YSF) >= 0;

    if (pairs_with_dmr) {
        dmr = media_peer_bus_primary_dmr(core->bus);
        if (dmr && peer_dmr_connected(dmr)) {
            if (slot->pcm_tx_dmr.ending)
                pcm_dmr_pace_end(core, kind, slot);
            else
                pcm_dmr_pace_tx(core, kind, slot);
        }
    }

    if (pairs_with_ysf && slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER
        && bridge_ms_elapsed(&slot->pcm_tx_ysf.last_tx, YSF_FRAME_MS))
        (void)pcm_emit_ysf_from_conv(core, kind, slot);

    if (bridge_ms_since(&slot->pcm_capture.last_speech) >= pcm_capture_hang_ms(kind)) {
        if (slot->pcm_tx_dmr.phase == MEDIA_CALL_TX_TO_PEER && !slot->pcm_tx_dmr.ending)
            pcm_dmr_begin_end(kind, slot);
        if (slot->pcm_tx_ysf.phase == MEDIA_CALL_TX_TO_PEER && !slot->pcm_tx_ysf.ending)
            pcm_end_ysf_call(kind, slot);
    }

    if (slot->pcm_rx.phase == MEDIA_CALL_RX_FROM_PEER) {
        if (slot->pcm_rx.rx_src_kind == MEDIA_PEER_DMR
            && bridge_ms_since(&slot->pcm_rx.last_rx) >= DMR_RX_HANG_MS) {
            pcm_flush_out(kind, slot);
            LOG_DMR_INFO("%s call end (%d voice frames in) — RX hangtime\n",
                         media_flow_label(MEDIA_PEER_DMR, kind), slot->pcm_rx.dmr_voice_frames);
            core_router_release_active(core);
            slot->pcm_rx.phase = MEDIA_CALL_IDLE;
            slot->pcm_rx.dmr_voice_frames = 0;
            slot->pcm_rx.rx_stream_id = 0;
        } else if (slot->pcm_rx.rx_src_kind == MEDIA_PEER_YSF
                   && bridge_ms_since(&slot->pcm_rx.last_rx) >= YSF_RX_HANG_MS) {
            pcm_flush_out(kind, slot);
            pcm_set_talker_name(kind, slot, NULL);
            LOG_YSF_INFO("%s call end (%d voice frames in) — RX hangtime\n",
                         media_flow_label(MEDIA_PEER_YSF, kind), slot->pcm_rx.ysf_voice_frames);
            core_router_release_active(core);
            slot->pcm_rx.phase = MEDIA_CALL_IDLE;
            slot->pcm_rx.ysf_voice_frames = 0;
            modeconv_reset(slot->pcm_mc_ysf);
        }
    }
}

void core_pcm_bridge_tick(media_core_t *core, media_peer_kind_t pcm_kind)
{
    int i;

    if (!core->router)
        return;
    if (media_router_find_first(core->router, MEDIA_PEER_DMR) >= 0)
        core_pcm_dmr_poll_connect_ptt(core, pcm_kind);
    if (!core->bus)
        return;
    for (i = 0; i < core->bus->n_slots; i++) {
        media_peer_slot_t *slot = &core->bus->slots[i];

        if (slot->open && slot->kind == pcm_kind)
            pcm_tick_one(core, pcm_kind, slot);
    }
}

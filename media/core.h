/*
 * media_core — unified session/router brain for all bridge layouts.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_CORE_H
#define ADN_MEDIA_CORE_H

#include <stdint.h>
#include <time.h>

#include "aliases.h"
#include "media/codec_plan.h"
#include "media/frame.h"
#include "media/peer_bus.h"
#include "media/router.h"
#include "mmdvm/modeconv_wrap.h"
#include "vocoder.h"

/* DMRA sidechain (talker alias) assembly — mirrors adn_bridge_t.dmra today. */
typedef struct {
    int     rf;
    uint8_t blocks[4][7];
    unsigned have;
    char    text[32];
} media_dmra_t;

/* media/core_ysf_dmr.c's own session state — a DMR source's call to a YSF
 * peer. Kept independent of media_leg_el_t below (own phase/call/counters/
 * timers) so it can run in the same tick as an EchoLink<->* call without one
 * pathway's pacing/counters corrupting the other's (a 3+-kind bus can have
 * both active from the same DMR source frame — see media_core_ingress). */
typedef struct {
    media_call_phase_t phase;
    media_call_meta_t  call; /* net_src/net_dst, talker_id, stream_id */

    /* DMR TX framing. */
    uint8_t dmr_seq;
    uint8_t dmr_last_dtype; /* gate duplicate VHEAD (YSF2DMR m_dmrLastDT) */
    int     dmr_voice_frames;
    int     dmr_tx_frames;

    /* YSF TX framing. */
    uint8_t ysf_cnt;
    int     ysf_voice_frames;

    struct timespec last_dmr_tx;
    struct timespec last_ysf_tx;
} media_leg_ysf_dmr_t;

/* PCM-native peer (EchoLink, ALSA, ...) <-> DMR/YSF session state used to
 * live here as media_leg_el_*_t, one fixed set of fields for "the" EchoLink
 * peer. It now lives generically on media_peer_slot_t instead (see
 * media/pcm_leg.h: media_pcm_capture_t/media_pcm_rx_leg_t/media_pcm_tx_leg_t)
 * so any number of PCM-native peer kinds/instances can cross to DMR/YSF via
 * the single generic media/core_pcm_bridge.c, each with its own state, none
 * of it shared -- exactly the media_leg_ysf_dmr_t rationale above, applied
 * per-peer-instance instead of per-mode. */

typedef struct {
    /* Infra (bound once at engine start). */
    media_router_t           *router;
    media_peer_bus_t         *bus;
    const media_codec_plan_t *plan;
    adn_bridge_aliases_t     *aliases;

    /* Each PCM-native peer opens its own vocoder connection on its own slot
     * (media_peer_slot_t.pcm_voc/pcm_voc_ready) instead of one shared here —
     * see media/core_pcm_bridge.c's design notes on why two such peers must
     * never share one vocoder_t. */

    /* ModeConv is a stateful ring-buffer transcoder, one instance per
     * concurrent cross-kind pairing — sharing one across pairings would let
     * two calls in the same tick (a 3+-kind bus) corrupt each other's ring
     * buffer. mc_ysf_dmr: media/core_ysf_dmr.c's DMR<->YSF direct pathway.
     * Every PCM-native peer's own ModeConv instances (mic->DMR, mic-or-
     * RX<->YSF) live on ITS OWN media_peer_slot_t instead (see
     * media/pcm_leg.h / media/core_pcm_bridge.c) — same reasoning, applied
     * per-peer-instance so 2+ PCM peers never share a ring buffer either. */
    modeconv_t *mc_ysf_dmr;

    /* Per-pathway session state — see media_leg_ysf_dmr_t above. PCM-native
     * peers' own session state (formerly media_leg_el_{capture,rx,tx_dmr,
     * tx_ysf}_t here) now lives on their own media_peer_slot_t instead. */
    media_leg_ysf_dmr_t    leg_ysf_dmr;

    uint8_t dmr_slot_bit; /* always 0x80 = TS2 */
    int     bridge_dmrid; /* [peer.*] dmr dmrid — fallback RF id / alias miss, valid
                            * even in a layout where no DMR peer exists */

    /* Connect-PTT state lives per-slot now (media_peer_slot_t.cp_* /
     * clear_dynamic_tg / dmr_was_connected) — each DMR destination may have
     * its own TG and its own clear_dynamic_tg config, so one shared TG fanned
     * out to every destination was wrong with >1 DMR peer. See
     * media/core_pcm_bridge.c and media/core_ysf_dmr.c. */

    /* Same-protocol relay (media/core_relay.c) — no phase/call of its own;
     * DMR/YSF relay track "who's active" via the router's shared
     * active_ingress instead, so relay composes safely alongside a
     * concurrent DMR<->YSF or EchoLink<->* call in a 3+-kind bus.
     * relay_last_dmr_tx/relay_last_ysf_tx are write-only scratch required by
     * dmr_tx_args_t/ysf_tx_args_t's API contract (nothing reads them back —
     * relay has no tick-based pacing to gate). */
    struct timespec last_el_relay_speech;
    struct timespec relay_last_dmr_tx;
    struct timespec relay_last_ysf_tx;
    /* Stamped on every relayed CALL_BEGIN/VOICE frame while that peer holds
     * the router's active_ingress -- lets media_core_tick's stale-call check
     * detect a lost VTERM/EOT (e.g. dropped on a lossy RF/hotspot link) and
     * force-release the lock instead of blocking every other peer forever. */
    struct timespec last_dmr_relay_rx;
    struct timespec last_ysf_relay_rx;
    /* Talker identity captured at relay CALL_BEGIN, needed to address the
     * synthetic VTERM/TERMINATOR sent when core_relay_check_stale fires. */
    media_call_meta_t relay_dmr_meta;
    media_call_meta_t relay_ysf_meta;

    /* DMRA sidechain (talker identity). */
    media_dmra_t dmra;
} media_core_t;

void media_core_init(media_core_t *core);
void media_core_bind(media_core_t *core, media_router_t *router, media_peer_bus_t *bus,
                     const media_codec_plan_t *plan, adn_bridge_aliases_t *aliases);
void media_core_set_bridge_dmrid(media_core_t *core, int dmrid);

/* Dispatch to the right pathway module by resolving src/dst codec via router +
 * codec_pair_resolve — no ADN_BRIDGE_LAYOUT_* switch (media/core.c). */
void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void media_core_tick(media_core_t *core);
/* A PCM-native peer kind (EchoLink, ALSA, ...) is polled directly, not
 * wire-classified — call once per enabled PCM kind after polling that
 * peer's own tick/poll (see media/core_pcm_bridge.c). */
void media_core_poll_pcm(media_core_t *core, media_peer_kind_t pcm_kind);

#endif

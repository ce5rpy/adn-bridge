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

typedef enum {
    MEDIA_CALL_IDLE = 0,
    MEDIA_CALL_TX_TO_PEER,   /* ingress from one peer, fanning out to others */
    MEDIA_CALL_RX_FROM_PEER, /* single-peer layouts: peer -> bus, symmetric leg */
} media_call_phase_t;

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

/* media/core_echolink.c's own session state — EchoLink<->DMR and
 * EchoLink<->YSF share this (never concurrent with each other: a single EL
 * leg is half-duplex, only one direction/peer at a time), but never with
 * media_leg_ysf_dmr_t above. */
typedef struct {
    media_call_phase_t phase;
    media_call_meta_t  call;

    uint8_t  dmr_seq;
    int      dmr_voice_frames;
    uint32_t dmr_rx_stream_id; /* DMR->EL RX stream (dedupe VHEAD), separate
                                 * from call.stream_id which is the EL->DMR TX
                                 * stream */

    /* EchoLink PCM accumulation + AMBE grouping before ModeConv/vocoder flush. */
    int16_t pcm_el_acc[160];
    int     pcm_el_acc_n;
    uint8_t el_ambe_buf[3][7];
    int     el_ambe_count;
    uint8_t ysf_ambe_buf[5][7];
    int     ysf_ambe_count;

    uint8_t ysf_cnt;
    int     ysf_voice_frames;

    struct timespec last_dmr_tx;
    struct timespec last_dmr_rx;
    struct timespec last_ysf_tx;
    struct timespec last_el_speech;
    struct timespec last_el_tx_end;
    int el_speech_run;
    int dmr_ending;
    int ysf_ending;
} media_leg_el_t;

typedef struct {
    /* Infra (bound once at engine start). */
    media_router_t           *router;
    media_peer_bus_t         *bus;
    const media_codec_plan_t *plan;
    adn_bridge_aliases_t     *aliases;

    /* Vocoder — opened/used only when plan->needs_vocoder. */
    vocoder_t voc;
    int       use_vocoder;

    /* ModeConv is a stateful ring-buffer transcoder, one instance per
     * concurrent cross-kind pairing — sharing one across pairings would let
     * two calls in the same tick (a 3+-kind bus) corrupt each other's ring
     * buffer. mc_ysf_dmr: media/core_ysf_dmr.c's DMR<->YSF direct pathway.
     * mc_el: media/core_echolink.c's EchoLink<->DMR/YSF vocoder pathway (its
     * two sub-cases don't run concurrently with each other, only one EL leg
     * can be active at a time, so they share one instance). */
    modeconv_t *mc_ysf_dmr;
    modeconv_t *mc_el;

    /* Per-pathway session state — see media_leg_ysf_dmr_t/media_leg_el_t. */
    media_leg_ysf_dmr_t leg_ysf_dmr;
    media_leg_el_t      leg_el;

    uint8_t dmr_slot_bit; /* always 0x80 = TS2 */
    float   el_pcm_gain;  /* [peer.*] echolink gain — EL->DMR/YSF PCM scale */
    int     bridge_dmrid; /* [peer.*] dmr dmrid — fallback RF id / alias miss, valid
                            * even in EL<->YSF layout where no DMR peer exists */

    /* Connect-PTT state lives per-slot now (media_peer_slot_t.cp_* /
     * clear_dynamic_tg / dmr_was_connected) — each DMR destination may have
     * its own TG and its own clear_dynamic_tg config, so one shared TG fanned
     * out to every destination was wrong with >1 DMR peer. See
     * media/core_echolink.c and media/core_ysf_dmr.c. */

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
/* Clamp like bridge_el_init: (0, 4] valid, else unity gain. */
void media_core_set_el_gain(media_core_t *core, float gain);
void media_core_set_bridge_dmrid(media_core_t *core, int dmrid);

/* Dispatch to the right pathway module by resolving src/dst codec via router +
 * codec_pair_resolve — no ADN_BRIDGE_LAYOUT_* switch (media/core.c). */
void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void media_core_tick(media_core_t *core);
/* EL PCM is polled directly (not wire-classified) — call after polling the EL peer. */
void media_core_poll_el_pcm(media_core_t *core);

#endif

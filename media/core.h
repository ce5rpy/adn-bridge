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

typedef struct {
    /* Infra (bound once at engine start). */
    media_router_t           *router;
    media_peer_bus_t         *bus;
    const media_codec_plan_t *plan;
    adn_bridge_aliases_t     *aliases;

    /* Vocoder — opened/used only when plan->needs_vocoder. */
    vocoder_t voc;
    int       use_vocoder;

    /* Ingress/session state. */
    int                 ingress_router_id; /* router id of the peer currently keyed, or -1 */
    media_call_phase_t  phase;
    media_call_meta_t   call;              /* net_src/net_dst, talker_id, stream_id */

    /* DMR TX framing. */
    uint8_t dmr_seq;
    uint8_t dmr_slot_bit; /* always 0x80 = TS2 */
    int     dmr_voice_frames;
    int     dmr_tx_frames;

    /* YSF TX framing. */
    uint8_t ysf_fn;
    uint8_t ysf_cnt;
    int     ysf_voice_frames;

    /* EchoLink PCM accumulation + AMBE grouping before ModeConv/vocoder flush. */
    int16_t pcm_el_acc[160];
    int     pcm_el_acc_n;
    uint8_t el_ambe_buf[3][7];
    int     el_ambe_count;
    uint8_t ysf_ambe_buf[5][7];
    int     ysf_ambe_count;

    /* Timers: hang / pacing / cooldown / connect-PTT. */
    struct timespec last_dmr_tx;
    struct timespec last_dmr_rx;
    struct timespec last_ysf_tx;
    struct timespec last_el_speech;
    struct timespec last_el_tx_end;
    int el_speech_run;
    int dmr_ending;
    int ysf_ending;

    /* Connect-PTT (TG 4000 optional clear + activation TG). */
    int dmr_was_connected;
    int clear_dynamic_tg;
    int connect_ptt_active;
    int connect_ptt_phase;
    int connect_ptt_voice_frames;
    int connect_ptt_tg;
    int connect_ptt_clearing;
    struct timespec connect_ptt_start;

    /* DMRA sidechain (talker identity). */
    media_dmra_t dmra;
} media_core_t;

void media_core_init(media_core_t *core);
void media_core_bind(media_core_t *core, media_router_t *router, media_peer_bus_t *bus,
                     const media_codec_plan_t *plan, adn_bridge_aliases_t *aliases);

/* Fase 3 rellena el cuerpo real (transform + fan-out); por ahora sólo aplica
 * media_router_ingress_allowed y no transforma ni reenvía nada. */
void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void media_core_tick(media_core_t *core);

#endif

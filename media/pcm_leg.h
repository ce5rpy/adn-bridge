/*
 * Generic per-slot session state for any PCM-native peer (EchoLink, ALSA, or
 * a future one) crossing to DMR/YSF via media/core_pcm_bridge.c.
 *
 * Lives on media_peer_slot_t (media/peer_bus.h), never on media_core_t — two
 * PCM peers active at once must never share this (Fase 7 lesson: sharing
 * session state between two concurrently-active crossings corrupted both).
 * Adding a new PCM peer kind needs zero new core state: it just gets its own
 * copy of these fields on its own slot.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_PCM_LEG_H
#define ADN_MEDIA_PCM_LEG_H

#include <stdint.h>
#include <time.h>

#include "codecs/codec.h"
#include "media/frame.h"
#include "media/router.h"

/* Physical/software capture accumulator + hang timer. One per PCM peer
 * instance -- shared by both destination legs below (same underlying audio
 * regardless of which destination(s) consume it), but never shared across
 * peer instances. */
typedef struct {
    int16_t pcm_acc[CODEC_PCM_SAMPLES];
    int     pcm_acc_n;
    struct timespec last_speech; /* last time a full 160-sample chunk arrived */
} media_pcm_capture_t;

/* Network (DMR or YSF) -> this PCM peer's playback. Single listener/single
 * active source at a time, arbitrated by the router's shared active_ingress
 * exactly like every other pathway. rx_src_kind records which category
 * actually claimed it, so tick-time hang detection doesn't have to guess. */
typedef struct {
    media_call_phase_t phase; /* IDLE or RX_FROM_PEER */
    media_call_meta_t  call;
    media_peer_kind_t  rx_src_kind;
    uint32_t rx_stream_id; /* DMR->peer RX stream (dedupe VHEAD) */
    int      dmr_voice_frames;
    int      ysf_voice_frames;
    struct timespec last_rx; /* last peer(DMR or YSF)->this PCM peer activity */
} media_pcm_rx_leg_t;

/* This PCM peer's mic -> one AMBE-native destination kind (DMR or YSF).
 * Independent of the other destination's own tx leg -- both can be active
 * concurrently (mic audio reaching a DMR peer and a YSF peer at once).
 * ambe_buf[5] covers both groupings (DMR flushes every 3, YSF every 5). */
typedef struct {
    media_call_phase_t phase; /* IDLE or TX_TO_PEER */
    media_call_meta_t  call;
    uint8_t  ambe_buf[5][7];
    int      ambe_count;
    int      voice_frames;
    uint8_t  frame_cnt;   /* dmr_seq (DMR dest) or ysf_cnt (YSF dest) */
    struct timespec last_tx;     /* pacing */
    struct timespec last_tx_end; /* cooldown */
    int      speech_run;
    int      ending;
} media_pcm_tx_leg_t;

#endif

/*
 * EchoLink <-> DMR/YSF voice bridge (PCM + remote vocoder).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef BRIDGE_EL_H
#define BRIDGE_EL_H

#include <stdint.h>
#include <time.h>

#include "aliases.h"
#include "media/peer_bus.h"
#include "media/codec_plan.h"
#include "media/router.h"
#include "peer_dmr.h"
#include "peer_echolink.h"
#include "peer_ysf.h"
#include "vocoder.h"

#define BRIDGE_EL_LINK_DMR 0
#define BRIDGE_EL_LINK_YSF  1

typedef struct {
    media_peer_bus_t *bus;
    peer_echolink_t *el;
    peer_dmr_t *dmr;
    peer_ysf_t *ysf;
    int ingress_router_id;
    vocoder_t voc;
    int use_vocoder; /* set from media_codec_plan — skip vocoder I/O when 0 */
    adn_bridge_aliases_t *aliases;
    int link_kind; /* BRIDGE_EL_LINK_DMR or BRIDGE_EL_LINK_YSF */
    int bridge_dmrid; /* [peer.*] dmr dmrid — fallback RF id / alias miss */
    int el_rf_id; /* EL→DMR talker RF id (alias of remote SDES talker) */
    uint32_t dmr_stream_id;    /* EL->DMR TX stream */
    uint32_t dmr_rx_stream_id; /* DMR->EL RX stream (dedupe VHEAD) */
    uint8_t dmr_seq;
    uint8_t dmr_slot_bit;
    int call_active; /* 1=EL->DMR/YSF, 2=DMR/YSF->EL */
    int dmr_voice_frames;
    int ysf_voice_frames;
    int el_ambe_count; /* 0..2 buffered before ModeConv flush (DMR) */
    uint8_t el_ambe_buf[3][7];
    int ysf_ambe_count; /* 0..4 before ModeConv flush (YSF VD2) */
    uint8_t ysf_ambe_buf[5][7];
    uint8_t ysf_cnt;
    char net_src[10];
    char net_dst[10];
    int16_t pcm_el_acc[160];
    int pcm_el_acc_n;
    float el_pcm_gain; /* [peer.*] echolink gain — EL→DMR/YSF PCM scale (1.0 = unity) */
    struct timespec last_dmr_tx;
    struct timespec last_dmr_rx; /* last DMRD/YSFD while peer->EL (call_active==2) */
    struct timespec last_ysf_tx;
    struct timespec last_el_speech; /* last inbound EL PCM (hang / activity) */
    struct timespec last_el_tx_end; /* when EL->DMR/YSF call fully ended (cooldown) */
    int el_speech_run; /* EL TX: start announced for current attempt */
    int dmr_ending; /* paced superframe pad + VTERM in progress */
    int ysf_ending; /* ModeConv EOT queued; paced YSFD drain in progress */
    int dmr_was_connected;
    int clear_dynamic_tg; /* [dmr] clear_dynamic_tg — PTT 4000 before connect tg */
    int connect_ptt_active;
    int connect_ptt_phase;
    int connect_ptt_voice_frames;
    int connect_ptt_tg; /* DMRD dst while connect PTT active */
    int connect_ptt_clearing; /* 1 = current stage is TG 4000 */
    struct timespec connect_ptt_start;
    media_router_t *router;
} bridge_el_t;

void bridge_el_bind_router(bridge_el_t *b, media_router_t *router);
void bridge_el_attach_bus(bridge_el_t *b, media_peer_bus_t *bus);
void bridge_el_apply_codec_plan(bridge_el_t *b, const media_codec_plan_t *plan);
void bridge_el_init(bridge_el_t *b, int link_kind, const char *dmr_options,
                    adn_bridge_aliases_t *aliases, int bridge_dmrid,
                    float el_pcm_gain, int clear_dynamic_tg);
void bridge_el_on_dmrd_slot(bridge_el_t *b, int src_router_id, peer_dmr_t *dmr,
                            const uint8_t *pkt, int len);
void bridge_el_on_ysfd_slot(bridge_el_t *b, int src_router_id, peer_ysf_t *ysf,
                            const uint8_t *pkt, int len);
void bridge_el_tick(bridge_el_t *b);
/* Drain EL PCM through vocoder into DMR (call after peer_el_poll). */
void bridge_el_process_el_audio(bridge_el_t *b);
/* Drain EL PCM through vocoder into YSF (echolink-ysf). */
void bridge_el_process_el_to_ysf(bridge_el_t *b);
/* EchoLink callsign → space-padded YSF 10 chars (keeps -L/-R). */
void bridge_el_format_callsign10(char out[10], const char *src);

#endif

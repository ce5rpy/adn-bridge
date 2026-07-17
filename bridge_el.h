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
#include "peer_dmr.h"
#include "peer_echolink.h"
#include "peer_ysf.h"
#include "vocoder.h"

typedef struct {
    peer_echolink_t el;
    peer_dmr_t dmr;
    peer_ysf_t ysf; /* used only in echolink-ysf mode */
    vocoder_t voc;
    ysf2dmr_aliases_t *aliases;
    int mode; /* YSF2DMR_MODE_ECHOLINK_DMR or _YSF */
    int bridge_dmrid; /* INI [dmr] dmrid — YSF CSD/DCH when set */
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
    struct timespec last_dmr_tx;
    struct timespec last_dmr_rx; /* last DMRD/YSFD while peer->EL (call_active==2) */
    struct timespec last_ysf_tx;
    struct timespec last_el_speech; /* last inbound EL PCM (hang / activity) */
    struct timespec last_el_tx_end; /* when EL->DMR/YSF call fully ended (cooldown) */
    int el_speech_run; /* EL TX: start announced for current attempt */
    int dmr_ending; /* paced superframe pad + VTERM in progress */
    int ysf_ending; /* ModeConv EOT queued; paced YSFD drain in progress */
    int dmr_was_connected;
    int connect_ptt_active;
    int connect_ptt_phase;
    int connect_ptt_voice_frames;
    struct timespec connect_ptt_start;
} bridge_el_t;

void bridge_el_init(bridge_el_t *b, int mode, const char *dmr_options,
                    ysf2dmr_aliases_t *aliases, int bridge_dmrid);
void bridge_el_on_dmrd(bridge_el_t *b, const uint8_t *pkt, int len);
void bridge_el_on_ysfd(bridge_el_t *b, const uint8_t *pkt, int len);
void bridge_el_tick(bridge_el_t *b);
/* Drain EL PCM through vocoder into DMR (call after peer_el_poll). */
void bridge_el_process_el_audio(bridge_el_t *b);
/* Drain EL PCM through vocoder into YSF (echolink-ysf). */
void bridge_el_process_el_to_ysf(bridge_el_t *b);
/* EchoLink CE5RPY-L → space-padded YSF "CE5RPY    " (stop at - or /). */
void bridge_el_format_callsign10(char out[10], const char *src);

#endif

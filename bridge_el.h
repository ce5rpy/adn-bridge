/*
 * EchoLink <-> DMR voice bridge (PCM + remote vocoder).
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
    uint32_t dmr_stream_id;    /* EL->DMR TX stream */
    uint32_t dmr_rx_stream_id; /* DMR->EL RX stream (dedupe VHEAD) */
    uint8_t dmr_seq;
    uint8_t dmr_slot_bit;
    int call_active; /* 1=EL->DMR, 2=DMR->EL */
    int dmr_voice_frames;
    int el_ambe_count; /* 0..2 buffered before ModeConv flush */
    uint8_t el_ambe_buf[3][7];
    int16_t pcm_el_acc[160];
    int pcm_el_acc_n;
    struct timespec last_dmr_tx;
    int dmr_was_connected;
    int connect_ptt_active;
    int connect_ptt_phase;
    int connect_ptt_voice_frames;
    struct timespec connect_ptt_start;
} bridge_el_t;

void bridge_el_init(bridge_el_t *b, int mode, const char *dmr_options,
                    ysf2dmr_aliases_t *aliases);
void bridge_el_on_dmrd(bridge_el_t *b, const uint8_t *pkt, int len);
void bridge_el_tick(bridge_el_t *b);
/* Drain EL PCM through vocoder into DMR (call after peer_el_poll). */
void bridge_el_process_el_audio(bridge_el_t *b);

#endif

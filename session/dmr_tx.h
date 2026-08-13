/*
 * Shared DMRD TX build/send (bridge.c + bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_DMR_TX_H
#define ADN_DMR_TX_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "peer_dmr.h"

typedef struct {
    peer_dmr_t *peer;
    int bridge_dmrid;
    int talker_rf_id;
    int tx_tg;
    uint8_t *seq;
    uint32_t stream_id;
    struct timespec *last_tx;
    bool *emb_raw; /* this destination's own scratch, see dmr_codec.h */
} dmr_tx_args_t;

void dmr_tx_send(const dmr_tx_args_t *a, uint8_t frame_type, const uint8_t *voice33);

#endif

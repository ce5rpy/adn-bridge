/*
 * EchoLink wire adapter: PCM read/write only, hand off to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_EL_H
#define ADAPTER_EL_H

#include <stdint.h>

#include "peer_echolink.h"

/* Read up to max_samples of inbound PCM (8 kHz s16 LE). Returns samples read. */
int adapter_el_read_pcm(peer_echolink_t *el, int16_t *pcm, int max_samples);
/* Queue 160 samples of PCM for TX to the EL peer. */
int adapter_el_egress_pcm(peer_echolink_t *el, const int16_t pcm[160]);

#endif

/*
 * ALSA wire adapter: PCM read/write only, hand off to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_ALSA_H
#define ADAPTER_ALSA_H

#include <stdint.h>

#include "peer_alsa.h"

/* Read up to max_samples of inbound PCM (8 kHz s16 LE). Returns samples read. */
int adapter_alsa_read_pcm(peer_alsa_t *a, int16_t *pcm, int max_samples);
/* Write `samples` of PCM to the ALSA playback device. */
int adapter_alsa_egress_pcm(peer_alsa_t *a, const int16_t *pcm, int samples);

#endif

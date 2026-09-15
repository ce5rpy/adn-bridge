/*
 * ALSA wire adapter: PCM read/write only, hand off to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/alsa.h"

int adapter_alsa_read_pcm(peer_alsa_t *a, int16_t *pcm, int max_samples)
{
    return peer_alsa_read_pcm(a, pcm, max_samples);
}

int adapter_alsa_egress_pcm(peer_alsa_t *a, const int16_t *pcm, int samples)
{
    return peer_alsa_write_pcm(a, pcm, samples);
}

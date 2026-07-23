/*
 * EchoLink wire adapter: PCM read/write only, hand off to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/el.h"

int adapter_el_read_pcm(peer_echolink_t *el, int16_t *pcm, int max_samples)
{
    return peer_el_read_pcm(el, pcm, max_samples);
}

int adapter_el_egress_pcm(peer_echolink_t *el, const int16_t *pcm, int samples)
{
    return peer_el_write_pcm(el, pcm, samples);
}

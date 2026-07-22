/*
 * PCM pass-through codec (8 kHz, 20 ms frames).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "codecs/pcm.h"

#include <string.h>

int codec_pcm_copy(const int16_t src[CODEC_PCM_SAMPLES], int16_t dst[CODEC_PCM_SAMPLES])
{
    if (!src || !dst)
        return 0;
    memcpy(dst, src, CODEC_PCM_SAMPLES * sizeof(int16_t));
    return 1;
}

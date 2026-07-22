/*
 * PCM pass-through codec (8 kHz, 20 ms frames).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CODEC_PCM_H
#define ADN_CODEC_PCM_H

#include <stdint.h>

#include "codecs/codec.h"

int codec_pcm_copy(const int16_t src[CODEC_PCM_SAMPLES], int16_t dst[CODEC_PCM_SAMPLES]);

#endif

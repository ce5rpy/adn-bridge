/*
 * DMR 49-bit AMBE via remote vocoder (DV3000).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CODEC_DMR_AMBE_H
#define ADN_CODEC_DMR_AMBE_H

#include <stdint.h>

#include "codecs/codec.h"
#include "vocoder.h"

int codec_dmr_ambe_encode(vocoder_t *voc, const int16_t pcm[CODEC_PCM_SAMPLES],
                          uint8_t ambe[CODEC_DMR_AMBE_BYTES]);
int codec_dmr_ambe_decode(vocoder_t *voc, const uint8_t ambe[CODEC_DMR_AMBE_BYTES],
                          int16_t pcm[CODEC_PCM_SAMPLES]);

#endif

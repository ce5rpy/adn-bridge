/*
 * Direct DMR AMBE ↔ YSF AMBE transcode via ModeConv (no PCM hub).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CODEC_PAIR_MODECONV_H
#define ADN_CODEC_PAIR_MODECONV_H

#include <stdint.h>

#include "codecs/codec.h"

int codec_pair_modeconv_available(codec_id_t src, codec_id_t dst);

unsigned int codec_pair_modeconv_dmr_to_ysf(uint8_t ysf120[CODEC_YSF_PAYLOAD_BYTES]);
unsigned int codec_pair_modeconv_ysf_to_dmr(uint8_t voice33[CODEC_DMR_VOICE_BYTES]);

#endif

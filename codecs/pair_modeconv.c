/*
 * Direct DMR AMBE ↔ YSF AMBE transcode via ModeConv (no PCM hub).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "codecs/pair_modeconv.h"

#include "codecs/registry.h"
#include "codecs/ysf_ambe.h"

int codec_pair_modeconv_available(codec_id_t src, codec_id_t dst)
{
    return codec_pair_resolve(src, dst) == CODEC_PAIR_DIRECT;
}

unsigned int codec_pair_modeconv_dmr_to_ysf(uint8_t ysf120[CODEC_YSF_PAYLOAD_BYTES])
{
    return codec_ysf_ambe_get_ysf(ysf120);
}

unsigned int codec_pair_modeconv_ysf_to_dmr(uint8_t voice33[CODEC_DMR_VOICE_BYTES])
{
    return codec_ysf_ambe_get_dmr(voice33);
}

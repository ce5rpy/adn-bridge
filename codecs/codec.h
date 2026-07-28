/*
 * Codec identifiers and frame sizes for the media bus.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CODEC_H
#define ADN_CODEC_H

#include <stdint.h>

#define CODEC_PCM_SAMPLES    160
#define CODEC_DMR_AMBE_BYTES 7
#define CODEC_DMR_VOICE_BYTES 33
#define CODEC_YSF_PAYLOAD_BYTES 120

typedef enum {
    CODEC_PCM = 0,
    CODEC_DMR_AMBE,
    CODEC_YSF_AMBE,
    CODEC_COUNT
} codec_id_t;

typedef enum {
    CODEC_PAIR_NONE = 0,
    CODEC_PAIR_RELAY,
    CODEC_PAIR_PCM,
    CODEC_PAIR_DIRECT,
} codec_pair_path_t;

#endif

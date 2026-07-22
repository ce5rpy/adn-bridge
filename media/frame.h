/*
 * Universal media bus frame (PCM hub).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_FRAME_H
#define ADN_MEDIA_FRAME_H

#include <stdint.h>

#include "codecs/codec.h"

typedef struct {
    int32_t  src_id;
    int32_t  dst_id;
    char     src_callsign[16];
    char     dst_callsign[16];
    uint8_t  slot;
    uint8_t  private_call;
    uint32_t stream_id;
} call_meta_t;

typedef enum {
    MEDIA_EVT_CALL_BEGIN,
    MEDIA_EVT_PCM,
    MEDIA_EVT_CALL_END,
} media_event_type_t;

typedef struct {
    media_event_type_t type;
    call_meta_t        meta;
    int16_t            pcm[CODEC_PCM_SAMPLES];
    uint64_t           ts_us;
} media_frame_t;

#endif

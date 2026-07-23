/*
 * Universal media bus frame — wire-agnostic unit adapters hand to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_FRAME_H
#define ADN_MEDIA_FRAME_H

#include <stdint.h>

#include "codecs/codec.h"
#include "media/call_meta.h"

typedef enum {
    MEDIA_FRAME_CALL_BEGIN,
    MEDIA_FRAME_CALL_END,
    MEDIA_FRAME_VOICE,
    MEDIA_FRAME_SIDECHAIN,   /* DMRA block, metadata only */
} media_frame_kind_t;

/* Wire callsign slots (bridge_call_meta_t) + the scalars adapters/core actually
 * use for routing and TX framing today (dmr.c/ysf.c/bridge_el.c). netcall is
 * filled by core once identity is actually resolved (only on call-begin, not
 * every voice frame) — adapters only carry the raw talker_id (rf) through. */
typedef struct {
    bridge_call_meta_t netcall;   /* net_src / net_dst, 10-char wire callsigns */
    int                talker_id; /* raw DMR radio ID from wire (rf) */
    uint32_t           stream_id;
} media_call_meta_t;

typedef struct {
    media_frame_kind_t kind;
    codec_id_t         codec;    /* CODEC_PCM | CODEC_DMR_AMBE | CODEC_YSF_AMBE */
    media_call_meta_t  meta;
    /* Protocol-specific wire bits core needs for exact session bookkeeping
     * (DMR b15 dtype gate on VOICE frames, wire seq for late-entry sync, raw
     * DMR dst/TG for identity net_dst formatting) — not part of the universal
     * contract, just carried through unparsed. */
    uint8_t            wire_dtype;
    uint8_t            wire_seq;
    int32_t            wire_dst;
    union {
        int16_t  pcm[CODEC_PCM_SAMPLES];
        uint8_t  dmr_voice33[CODEC_DMR_VOICE_BYTES];
        uint8_t  ysf_payload120[CODEC_YSF_PAYLOAD_BYTES];
        struct {
            int     rf;
            int     block_id;
            uint8_t block7[7];
        } dmra;
    } payload;
} media_bus_frame_t;

#endif

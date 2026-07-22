/*
 * YSF ↔ DMR AMBE path via MMDVM ModeConv (software transcoder).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CODEC_YSF_AMBE_H
#define ADN_CODEC_YSF_AMBE_H

#include <stdint.h>

#include "codecs/codec.h"

void codec_ysf_ambe_init(void);
void codec_ysf_ambe_reset(void);

void codec_ysf_ambe_put_dmr_voice(const uint8_t frame33[CODEC_DMR_VOICE_BYTES]);
void codec_ysf_ambe_put_dmr_header(void);
void codec_ysf_ambe_put_dmr_eot(void);
unsigned int codec_ysf_ambe_get_ysf(uint8_t ysf120[CODEC_YSF_PAYLOAD_BYTES]);

void codec_ysf_ambe_put_ysf_payload(const uint8_t ysf120[CODEC_YSF_PAYLOAD_BYTES]);
void codec_ysf_ambe_put_ysf_header(void);
void codec_ysf_ambe_put_ysf_eot(void);
unsigned int codec_ysf_ambe_get_dmr(uint8_t voice33[CODEC_DMR_VOICE_BYTES]);

void codec_ysf_ambe_put_ambe7_dmr(const uint8_t ambe7[CODEC_DMR_AMBE_BYTES]);
void codec_ysf_ambe_put_ambe7_ysf(const uint8_t ambe7[CODEC_DMR_AMBE_BYTES]);
void codec_ysf_ambe_dmr33_to_ambe(const uint8_t dmr33[CODEC_DMR_VOICE_BYTES],
                                  uint8_t ambe[3][CODEC_DMR_AMBE_BYTES]);

#endif

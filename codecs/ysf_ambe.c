/*
 * YSF ↔ DMR AMBE path via MMDVM ModeConv (software transcoder).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "codecs/ysf_ambe.h"

#include "mmdvm/modeconv_wrap.h"

void codec_ysf_ambe_init(void)
{
    modeconv_init();
}

void codec_ysf_ambe_reset(void)
{
    modeconv_reset();
}

void codec_ysf_ambe_put_dmr_voice(const uint8_t frame33[CODEC_DMR_VOICE_BYTES])
{
    modeconv_put_dmr_voice(frame33);
}

void codec_ysf_ambe_put_dmr_header(void)
{
    modeconv_put_dmr_header();
}

void codec_ysf_ambe_put_dmr_eot(void)
{
    modeconv_put_dmr_eot();
}

unsigned int codec_ysf_ambe_get_ysf(uint8_t ysf120[CODEC_YSF_PAYLOAD_BYTES])
{
    return modeconv_get_ysf(ysf120);
}

void codec_ysf_ambe_put_ysf_payload(const uint8_t ysf120[CODEC_YSF_PAYLOAD_BYTES])
{
    modeconv_put_ysf_payload(ysf120);
}

void codec_ysf_ambe_put_ysf_header(void)
{
    modeconv_put_ysf_header();
}

void codec_ysf_ambe_put_ysf_eot(void)
{
    modeconv_put_ysf_eot();
}

unsigned int codec_ysf_ambe_get_dmr(uint8_t voice33[CODEC_DMR_VOICE_BYTES])
{
    return modeconv_get_dmr(voice33);
}

void codec_ysf_ambe_put_ambe7_dmr(const uint8_t ambe7[CODEC_DMR_AMBE_BYTES])
{
    modeconv_put_ambe7(ambe7);
}

void codec_ysf_ambe_put_ambe7_ysf(const uint8_t ambe7[CODEC_DMR_AMBE_BYTES])
{
    modeconv_put_ambe7_ysf(ambe7);
}

void codec_ysf_ambe_dmr33_to_ambe(const uint8_t dmr33[CODEC_DMR_VOICE_BYTES],
                                  uint8_t ambe[3][CODEC_DMR_AMBE_BYTES])
{
    modeconv_dmr33_to_ambe(dmr33, ambe);
}

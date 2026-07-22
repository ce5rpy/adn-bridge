/*
 * Unit checks for codec registry and PCM path.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "codecs/codec.h"
#include "codecs/pcm.h"
#include "codecs/registry.h"

static void test_registry(void)
{
    assert(codec_is_registered(CODEC_PCM));
    assert(codec_is_registered(CODEC_DMR_AMBE));
    assert(codec_is_registered(CODEC_YSF_AMBE));
    assert(strcmp(codec_name(CODEC_PCM), "pcm") == 0);
    assert(codec_id_from_name("dmr_ambe") == CODEC_DMR_AMBE);
    assert(codec_id_from_name("ysf_ambe") == CODEC_YSF_AMBE);
    assert(codec_id_from_name("unknown") == CODEC_COUNT);
}

static void test_pair_resolve(void)
{
    assert(codec_pair_resolve(CODEC_PCM, CODEC_PCM) == CODEC_PAIR_RELAY);
    assert(codec_pair_resolve(CODEC_DMR_AMBE, CODEC_DMR_AMBE) == CODEC_PAIR_RELAY);
    assert(codec_pair_resolve(CODEC_DMR_AMBE, CODEC_YSF_AMBE) == CODEC_PAIR_DIRECT);
    assert(codec_pair_resolve(CODEC_YSF_AMBE, CODEC_DMR_AMBE) == CODEC_PAIR_DIRECT);
    assert(codec_pair_resolve(CODEC_PCM, CODEC_DMR_AMBE) == CODEC_PAIR_PCM);
    assert(codec_pair_resolve(CODEC_DMR_AMBE, CODEC_PCM) == CODEC_PAIR_PCM);
}

static void test_pcm_copy(void)
{
    int16_t src[CODEC_PCM_SAMPLES];
    int16_t dst[CODEC_PCM_SAMPLES];
    int i;

    for (i = 0; i < CODEC_PCM_SAMPLES; i++)
        src[i] = (int16_t)(i - 80);
    memset(dst, 0, sizeof(dst));
    assert(codec_pcm_copy(src, dst));
    assert(memcmp(src, dst, sizeof(src)) == 0);
}

int main(void)
{
    test_registry();
    test_pair_resolve();
    test_pcm_copy();
    printf("test_codecs: ok\n");
    return 0;
}

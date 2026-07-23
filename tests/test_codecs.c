/*
 * Unit checks for codec registry and PCM path.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>

#include "codecs/codec.h"
#include "codecs/registry.h"

static void test_registry(void)
{
    assert(codec_is_registered(CODEC_PCM));
    assert(codec_is_registered(CODEC_DMR_AMBE));
    assert(codec_is_registered(CODEC_YSF_AMBE));
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

int main(void)
{
    test_registry();
    test_pair_resolve();
    printf("test_codecs: ok\n");
    return 0;
}

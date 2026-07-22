/*
 * Codec registry — lookup by id or name.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "codecs/registry.h"

#include <string.h>

static const char *const codec_names[CODEC_COUNT] = {
    [CODEC_PCM] = "pcm",
    [CODEC_DMR_AMBE] = "dmr_ambe",
    [CODEC_YSF_AMBE] = "ysf_ambe",
};

const char *codec_name(codec_id_t id)
{
    if (id < 0 || id >= CODEC_COUNT)
        return NULL;
    return codec_names[id];
}

codec_id_t codec_id_from_name(const char *name)
{
    int i;

    if (!name || !name[0])
        return CODEC_COUNT;
    for (i = 0; i < CODEC_COUNT; i++) {
        if (codec_names[i] && strcmp(name, codec_names[i]) == 0)
            return (codec_id_t)i;
    }
    return CODEC_COUNT;
}

int codec_is_registered(codec_id_t id)
{
    return id >= 0 && id < CODEC_COUNT && codec_names[id] != NULL;
}

codec_pair_path_t codec_pair_resolve(codec_id_t src, codec_id_t dst)
{
    if (src == dst) {
        if (codec_is_registered(src))
            return CODEC_PAIR_RELAY;
        return CODEC_PAIR_NONE;
    }
    if ((src == CODEC_DMR_AMBE && dst == CODEC_YSF_AMBE)
        || (src == CODEC_YSF_AMBE && dst == CODEC_DMR_AMBE)) {
        return CODEC_PAIR_DIRECT;
    }
    if (codec_is_registered(src) && codec_is_registered(dst))
        return CODEC_PAIR_PCM;
    return CODEC_PAIR_NONE;
}

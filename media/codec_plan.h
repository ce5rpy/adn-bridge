/*
 * Startup codec / resource plan from enabled peers on the bus.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_CODEC_PLAN_H
#define ADN_MEDIA_CODEC_PLAN_H

#include "codecs/codec.h"
#include "config.h"
#include "media/router.h"

typedef struct {
    int needs_vocoder;
    int needs_modeconv;
    int has_dmr;
    int has_ysf;
    int has_el;
    int enabled_peers;
} media_codec_plan_t;

void media_codec_plan_build(const media_router_t *router, media_codec_plan_t *plan);
int media_codec_plan_from_config(const adn_bridge_config_t *cfg, media_codec_plan_t *plan);

#endif

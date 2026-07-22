/*
 * Codec registry — lookup by id or name.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CODEC_REGISTRY_H
#define ADN_CODEC_REGISTRY_H

#include "codecs/codec.h"

const char *codec_name(codec_id_t id);
codec_id_t codec_id_from_name(const char *name);
int codec_is_registered(codec_id_t id);

codec_pair_path_t codec_pair_resolve(codec_id_t src, codec_id_t dst);

#endif

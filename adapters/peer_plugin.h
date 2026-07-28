/*
 * Peer kind plugins — wire codec and resource needs per protocol.
 *
 * New modes register a row here; core fan-out/codec_plan read this table only.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_PEER_PLUGIN_H
#define ADN_PEER_PLUGIN_H

#include "codecs/codec.h"
#include "config.h"
#include "media/router.h"

typedef struct {
    adn_bridge_peer_type_t config_type;
    media_peer_kind_t     router_kind;
    codec_id_t            wire_codec;
} peer_kind_plugin_t;

const peer_kind_plugin_t *peer_plugin_for_config_type(adn_bridge_peer_type_t type);
codec_id_t peer_plugin_wire_codec(media_peer_kind_t kind);
const char *peer_plugin_label(media_peer_kind_t kind);

#endif

/*
 * Peer kind plugin table (DMR / YSF / EchoLink).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/peer_plugin.h"

#include <stddef.h>

static const peer_kind_plugin_t peer_plugins[] = {
    {
        ADN_BRIDGE_PEER_TYPE_DMR,
        MEDIA_PEER_DMR,
        CODEC_DMR_AMBE,
        0,
    },
    {
        ADN_BRIDGE_PEER_TYPE_YSF,
        MEDIA_PEER_YSF,
        CODEC_YSF_AMBE,
        0,
    },
    {
        ADN_BRIDGE_PEER_TYPE_ECHOLINK,
        MEDIA_PEER_ECHOLINK,
        CODEC_PCM,
        1,
    },
};

static const peer_kind_plugin_t *peer_plugin_find_config(adn_bridge_peer_type_t type)
{
    size_t i;

    for (i = 0; i < sizeof(peer_plugins) / sizeof(peer_plugins[0]); i++) {
        if (peer_plugins[i].config_type == type)
            return &peer_plugins[i];
    }
    return NULL;
}

const peer_kind_plugin_t *peer_plugin_for_config_type(adn_bridge_peer_type_t type)
{
    return peer_plugin_find_config(type);
}

const peer_kind_plugin_t *peer_plugin_for_router_kind(media_peer_kind_t kind)
{
    size_t i;

    for (i = 0; i < sizeof(peer_plugins) / sizeof(peer_plugins[0]); i++) {
        if (peer_plugins[i].router_kind == kind)
            return &peer_plugins[i];
    }
    return NULL;
}

codec_id_t peer_plugin_wire_codec(media_peer_kind_t kind)
{
    const peer_kind_plugin_t *p = peer_plugin_for_router_kind(kind);

    return p ? p->wire_codec : CODEC_COUNT;
}

const char *peer_plugin_label(media_peer_kind_t kind)
{
    switch (kind) {
    case MEDIA_PEER_DMR:
        return "dmr";
    case MEDIA_PEER_YSF:
        return "ysf";
    case MEDIA_PEER_ECHOLINK:
        return "echolink";
    default:
        return "?";
    }
}

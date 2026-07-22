/*
 * Startup codec / resource plan from enabled peers on the bus.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/codec_plan.h"

#include "adapters/peer_plugin.h"
#include "codecs/registry.h"
#include "config.h"

static media_peer_kind_t plan_peer_kind(adn_bridge_peer_type_t type)
{
    switch (type) {
    case ADN_BRIDGE_PEER_TYPE_DMR:
        return MEDIA_PEER_DMR;
    case ADN_BRIDGE_PEER_TYPE_YSF:
        return MEDIA_PEER_YSF;
    case ADN_BRIDGE_PEER_TYPE_ECHOLINK:
        return MEDIA_PEER_ECHOLINK;
    default:
        return MEDIA_PEER_DMR;
    }
}

int media_codec_path_needs_vocoder(codec_pair_path_t path)
{
    return path == CODEC_PAIR_PCM;
}

int media_codec_path_needs_modeconv(codec_pair_path_t path)
{
    return path == CODEC_PAIR_DIRECT;
}

void media_codec_plan_build(const media_router_t *router, media_codec_plan_t *plan)
{
    int i, j;

    if (!plan)
        return;
    plan->needs_vocoder = 0;
    plan->needs_modeconv = 0;
    plan->has_dmr = 0;
    plan->has_ysf = 0;
    plan->has_el = 0;
    plan->enabled_peers = 0;

    if (!router)
        return;

    for (i = 0; i < router->n_peers; i++) {
        const media_router_peer_t *p = &router->peers[i];

        if (!p->enabled)
            continue;
        plan->enabled_peers++;
        switch (p->kind) {
        case MEDIA_PEER_DMR:
            plan->has_dmr = 1;
            break;
        case MEDIA_PEER_YSF:
            plan->has_ysf = 1;
            break;
        case MEDIA_PEER_ECHOLINK:
            plan->has_el = 1;
            break;
        default:
            break;
        }
    }

    for (i = 0; i < router->n_peers; i++) {
        if (!router->peers[i].enabled)
            continue;
        for (j = i + 1; j < router->n_peers; j++) {
            codec_pair_path_t path;

            if (!router->peers[j].enabled)
                continue;
            path = media_codec_plan_pair_path(router, i, j);
            if (media_codec_path_needs_vocoder(path))
                plan->needs_vocoder = 1;
            if (media_codec_path_needs_modeconv(path))
                plan->needs_modeconv = 1;
        }
    }
}

int media_codec_plan_from_config(const adn_bridge_config_t *cfg, media_codec_plan_t *plan)
{
    media_router_t router;
    int i;

    if (!cfg || !plan)
        return -1;
    media_router_init(&router);
    for (i = 0; i < cfg->peer_count; i++) {
        const adn_bridge_peer_t *p = &cfg->peers[i];

        if (!p->type_set)
            continue;
        if (media_router_add_peer_cfg(&router, plan_peer_kind(p->type), i, p->enabled) < 0)
            return -1;
    }
    media_codec_plan_build(&router, plan);
    return 0;
}

codec_pair_path_t media_codec_plan_pair_path(const media_router_t *router,
                                               int src_router_id, int dst_router_id)
{
    codec_id_t src_codec;
    codec_id_t dst_codec;

    if (!router || src_router_id < 0 || dst_router_id < 0
        || src_router_id >= router->n_peers || dst_router_id >= router->n_peers)
        return CODEC_PAIR_NONE;
    if (!router->peers[src_router_id].enabled || !router->peers[dst_router_id].enabled)
        return CODEC_PAIR_NONE;

    src_codec = peer_plugin_wire_codec(router->peers[src_router_id].kind);
    dst_codec = peer_plugin_wire_codec(router->peers[dst_router_id].kind);
    return codec_pair_resolve(src_codec, dst_codec);
}

/*
 * media_core — unified session/router brain for all bridge layouts.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core.h"

#include "adapters/peer_plugin.h"
#include "codecs/registry.h"
#include "media/core_echolink.h"
#include "media/core_ysf_dmr.h"

#include <string.h>

void media_core_init(media_core_t *core)
{
    if (!core)
        return;
    memset(core, 0, sizeof(*core));
    core->ingress_router_id = -1;
    core->phase = MEDIA_CALL_IDLE;
    core->dmr_slot_bit = 0x80; /* TX always TS2 */
    core->el_pcm_gain = 1.0f;
}

void media_core_bind(media_core_t *core, media_router_t *router, media_peer_bus_t *bus,
                     const media_codec_plan_t *plan, adn_bridge_aliases_t *aliases)
{
    if (!core)
        return;
    core->router = router;
    core->bus = bus;
    core->plan = plan;
    core->aliases = aliases;
    core->use_vocoder = plan && plan->needs_vocoder;
}

void media_core_set_el_gain(media_core_t *core, float gain)
{
    if (!core)
        return;
    core->el_pcm_gain = (gain > 0.0f && gain <= 4.0f) ? gain : 1.0f;
}

void media_core_set_bridge_dmrid(media_core_t *core, int dmrid)
{
    if (!core)
        return;
    core->bridge_dmrid = dmrid;
}

/* The other enabled peer kind sharing this bus with src_kind — with exactly
 * two peer kinds enabled (today's only supported configs), this is the one
 * codec_pair_resolve needs to pick a pathway; see media_core_ingress. */
static media_peer_kind_t core_other_enabled_kind(const media_core_t *core, media_peer_kind_t src_kind)
{
    int i;

    if (!core->router)
        return src_kind;
    for (i = 0; i < core->router->n_peers; i++) {
        if (!core->router->peers[i].enabled)
            continue;
        if (core->router->peers[i].kind != src_kind)
            return core->router->peers[i].kind;
    }
    return src_kind;
}

void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    media_peer_kind_t src_kind, dst_kind;
    codec_pair_path_t path;

    if (!core || !frame || !core->router)
        return;
    if (src_router_id < 0 || src_router_id >= core->router->n_peers)
        return;
    src_kind = core->router->peers[src_router_id].kind;

    if (frame->kind == MEDIA_FRAME_SIDECHAIN) {
        if (src_kind == MEDIA_PEER_DMR)
            core_ysf_dmr_merge_dmra(core, frame);
        return;
    }

    /* Router ingress arbitration is a force-take inside each pathway's begin
     * helper ("last keyed wins", half-duplex), not a blanket drop here. */
    dst_kind = core_other_enabled_kind(core, src_kind);
    path = codec_pair_resolve(peer_plugin_wire_codec(src_kind), peer_plugin_wire_codec(dst_kind));

    switch (path) {
    case CODEC_PAIR_DIRECT: /* YSF<->DMR: ModeConv shortcut, no PCM hub */
        if (src_kind == MEDIA_PEER_DMR)
            core_ysf_dmr_ingress_dmr(core, src_router_id, frame);
        else if (src_kind == MEDIA_PEER_YSF)
            core_ysf_dmr_ingress_ysf(core, src_router_id, frame);
        break;
    case CODEC_PAIR_PCM: /* EchoLink<->DMR or EchoLink<->YSF: vocoder */
        if (src_kind == MEDIA_PEER_DMR)
            core_el_dmr_ingress_dmr(core, src_router_id, frame);
        else if (src_kind == MEDIA_PEER_YSF)
            core_el_ysf_ingress_ysf(core, src_router_id, frame);
        break;
    default:
        break; /* CODEC_PAIR_RELAY/NONE: no pathway module implements these yet */
    }
}

void media_core_tick(media_core_t *core)
{
    int has_dmr, has_ysf, has_el;

    if (!core || !core->router)
        return;
    has_dmr = media_router_find_first(core->router, MEDIA_PEER_DMR) >= 0;
    has_ysf = media_router_find_first(core->router, MEDIA_PEER_YSF) >= 0;
    has_el = media_router_find_first(core->router, MEDIA_PEER_ECHOLINK) >= 0;

    if (has_dmr && has_ysf) {
        core_ysf_dmr_poll_connect_ptt(core);
        core_ysf_dmr_tick(core);
    } else if (has_el) {
        core_el_tick(core);
    }
}

/* EchoLink PCM ingress does not go through media_core_ingress (it is polled
 * directly, not classified from a wire packet) — exposed so engine.c can call
 * it after polling the EL peer, mirroring bridge_el_process_el_audio/
 * bridge_el_process_el_to_ysf today. */
void media_core_poll_el_pcm(media_core_t *core)
{
    int has_dmr, has_ysf;

    if (!core || !core->router)
        return;
    has_dmr = media_router_find_first(core->router, MEDIA_PEER_DMR) >= 0;
    has_ysf = media_router_find_first(core->router, MEDIA_PEER_YSF) >= 0;

    if (has_dmr)
        core_el_dmr_process_el_audio(core);
    else if (has_ysf)
        core_el_ysf_process_el_audio(core);
}

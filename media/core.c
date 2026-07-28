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
#include "media/core_relay.h"
#include "media/core_ysf_dmr.h"

#include <string.h>

void media_core_init(media_core_t *core)
{
    if (!core)
        return;
    memset(core, 0, sizeof(*core));
    core->dmr_slot_bit = 0x80; /* TX always TS2 */
    core->el_pcm_gain = 1.0f;
    core->mc_ysf_dmr = modeconv_create();
    core->mc_el = modeconv_create();
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

static const media_peer_kind_t CORE_ALL_KINDS[3] = {
    MEDIA_PEER_DMR, MEDIA_PEER_YSF, MEDIA_PEER_ECHOLINK,
};

/* >=2 enabled peers of `kind` — same-protocol relay only makes sense once a
 * second destination of the source's own kind exists. */
static int core_kind_has_multiple_enabled(const media_router_t *r, media_peer_kind_t kind)
{
    int i, n = 0;

    if (!r)
        return 0;
    for (i = 0; i < r->n_peers; i++) {
        if (r->peers[i].enabled && r->peers[i].kind == kind && ++n >= 2)
            return 1;
    }
    return 0;
}

/* Genuinely N-way: for every peer kind sharing this bus (including src's own,
 * "the diagonal"), resolve the pathway via codec_pair_resolve and run it if
 * reachable — not just the single "other kind" a 2-kind layout has room for.
 * Multiple families can fire for the same frame (e.g. a DMR source with both
 * another DMR peer and a YSF peer enabled relays to one and ModeConv's to the
 * other); they touch disjoint state so this is safe (see media/core_relay.c). */
void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    media_peer_kind_t src_kind;
    int i;

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

    for (i = 0; i < 3; i++) {
        media_peer_kind_t dst_kind = CORE_ALL_KINDS[i];
        codec_pair_path_t path;
        int reachable = (dst_kind == src_kind)
                         ? core_kind_has_multiple_enabled(core->router, dst_kind)
                         : media_router_find_first(core->router, dst_kind) >= 0;

        if (!reachable)
            continue;
        path = codec_pair_resolve(peer_plugin_wire_codec(src_kind), peer_plugin_wire_codec(dst_kind));
        switch (path) {
        case CODEC_PAIR_RELAY: /* same protocol both ends: wire-only fan-out */
            if (src_kind == MEDIA_PEER_DMR)
                core_relay_dmr_to_dmr(core, src_router_id, frame);
            else if (src_kind == MEDIA_PEER_YSF)
                core_relay_ysf_to_ysf(core, src_router_id, frame);
            break;
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
            break;
        }
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

    /* DMR HBP keepalive is independent of whatever else shares the bus (a
     * DMR-only or DMR-relay-only bus still needs it); core_el_tick also polls
     * it when has_el (its own copy, see media/core_echolink.c), which is
     * harmless to run alongside this — the second call in a tick is a no-op
     * (dmr_was_connected/last_dmr_tx already updated by whichever ran first). */
    if (has_dmr)
        core_ysf_dmr_poll_connect_ptt(core);
    if (has_dmr && has_ysf)
        core_ysf_dmr_tick(core);
    if (has_el)
        core_el_tick(core);
}

/* EchoLink PCM ingress does not go through media_core_ingress (it is polled
 * directly, not classified from a wire packet) — exposed so engine.c can call
 * it after polling the EL peer, mirroring bridge_el_process_el_audio/
 * bridge_el_process_el_to_ysf today. Same "run every applicable family"
 * approach as media_core_ingress: DMR and YSF fan-out are not mutually
 * exclusive in a 3-kind bus, and same-protocol EL<->EL relay is orthogonal
 * to both. */
void media_core_poll_el_pcm(media_core_t *core)
{
    int has_dmr, has_ysf;

    if (!core || !core->router)
        return;
    has_dmr = media_router_find_first(core->router, MEDIA_PEER_DMR) >= 0;
    has_ysf = media_router_find_first(core->router, MEDIA_PEER_YSF) >= 0;

    if (has_dmr)
        core_el_dmr_process_el_audio(core);
    if (has_ysf)
        core_el_ysf_process_el_audio(core);
    if (core_kind_has_multiple_enabled(core->router, MEDIA_PEER_ECHOLINK))
        core_relay_el_to_el(core);
}

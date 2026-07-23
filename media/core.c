/*
 * media_core — unified session/router brain for all bridge layouts.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core.h"

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
    core->layout = ADN_BRIDGE_LAYOUT_UNKNOWN;
}

void media_core_bind(media_core_t *core, adn_bridge_layout_t layout, media_router_t *router,
                     media_peer_bus_t *bus, const media_codec_plan_t *plan,
                     adn_bridge_aliases_t *aliases)
{
    if (!core)
        return;
    core->layout = layout;
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

void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    media_peer_kind_t kind;

    if (!core || !frame || !core->router)
        return;
    if (src_router_id < 0 || src_router_id >= core->router->n_peers)
        return;
    kind = core->router->peers[src_router_id].kind;

    /* Layout picks the pathway module — each of the 3 supported layouts is
     * exactly 2 peer kinds today (v0.3.1 parity bar); router ingress
     * arbitration is a force-take inside each pathway's begin helper ("last
     * keyed wins", half-duplex), not a blanket drop here. */
    switch (core->layout) {
    case ADN_BRIDGE_LAYOUT_YSF_DMR:
        if (kind == MEDIA_PEER_DMR) {
            if (frame->kind == MEDIA_FRAME_SIDECHAIN)
                core_ysf_dmr_merge_dmra(core, frame);
            else
                core_ysf_dmr_ingress_dmr(core, src_router_id, frame);
        } else if (kind == MEDIA_PEER_YSF) {
            core_ysf_dmr_ingress_ysf(core, src_router_id, frame);
        }
        break;
    case ADN_BRIDGE_LAYOUT_EL_DMR:
        if (kind == MEDIA_PEER_DMR)
            core_el_dmr_ingress_dmr(core, src_router_id, frame);
        break;
    case ADN_BRIDGE_LAYOUT_EL_YSF:
        if (kind == MEDIA_PEER_YSF)
            core_el_ysf_ingress_ysf(core, src_router_id, frame);
        break;
    default:
        break;
    }
}

void media_core_tick(media_core_t *core)
{
    if (!core)
        return;
    switch (core->layout) {
    case ADN_BRIDGE_LAYOUT_YSF_DMR:
        core_ysf_dmr_poll_connect_ptt(core);
        core_ysf_dmr_tick(core);
        break;
    case ADN_BRIDGE_LAYOUT_EL_DMR:
    case ADN_BRIDGE_LAYOUT_EL_YSF:
        core_el_tick(core);
        break;
    default:
        break;
    }
}

/* EchoLink PCM ingress does not go through media_core_ingress (it is polled
 * directly, not classified from a wire packet) — exposed so engine.c can call
 * it after adapter_el_poll_pcm, mirroring bridge_el_process_el_audio/
 * bridge_el_process_el_to_ysf today. */
void media_core_poll_el_pcm(media_core_t *core)
{
    if (!core)
        return;
    switch (core->layout) {
    case ADN_BRIDGE_LAYOUT_EL_DMR:
        core_el_dmr_process_el_audio(core);
        break;
    case ADN_BRIDGE_LAYOUT_EL_YSF:
        core_el_ysf_process_el_audio(core);
        break;
    default:
        break;
    }
}

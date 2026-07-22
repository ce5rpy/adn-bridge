/*
 * media_core — unified session/router brain for all bridge layouts.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core.h"

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

void media_core_ingress(media_core_t *core, int src_router_id, const media_bus_frame_t *frame)
{
    media_peer_kind_t kind;

    if (!core || !frame || !core->router)
        return;
    if (src_router_id < 0 || src_router_id >= core->router->n_peers)
        return;
    kind = core->router->peers[src_router_id].kind;

    /* Only the YSF<->DMR pathway is ported so far (Fase 3, EL pathways follow).
     * Router ingress arbitration is a force-take inside each pathway's begin
     * helper ("last keyed wins", half-duplex) — not a blanket drop here. */
    switch (kind) {
    case MEDIA_PEER_DMR:
        if (frame->kind == MEDIA_FRAME_SIDECHAIN)
            core_ysf_dmr_merge_dmra(core, frame);
        else
            core_ysf_dmr_ingress_dmr(core, src_router_id, frame);
        break;
    case MEDIA_PEER_YSF:
        core_ysf_dmr_ingress_ysf(core, src_router_id, frame);
        break;
    case MEDIA_PEER_ECHOLINK:
        break; /* siguiente commit: EL<->DMR/YSF */
    default:
        break;
    }
}

void media_core_tick(media_core_t *core)
{
    if (!core)
        return;
    core_ysf_dmr_poll_connect_ptt(core);
    core_ysf_dmr_tick(core);
}

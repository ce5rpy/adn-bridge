/*
 * media_core — unified session/router brain for all bridge layouts.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/core.h"

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
    if (!core || !frame)
        return;
    if (!media_router_ingress_allowed(core->router, src_router_id))
        return;
    /* Fase 3 rellena transform + fan-out. */
}

void media_core_tick(media_core_t *core)
{
    if (!core)
        return;
    /* Fase 3 rellena drain ModeConv / connect-PTT / hang / pacing. */
}

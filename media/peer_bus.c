/*
 * Runtime peer instances — one slot per router peer (multi-dest fan-out).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/peer_bus.h"

#include "adapters/peer_plugin.h"

#include <string.h>

void media_peer_bus_init(media_peer_bus_t *bus, const media_router_t *router)
{
    if (!bus)
        return;
    memset(bus, 0, sizeof(*bus));
    bus->router = router;
}

static media_peer_slot_t *bus_find_slot(media_peer_bus_t *bus, int router_id)
{
    int i;

    if (!bus || router_id < 0)
        return NULL;
    for (i = 0; i < bus->n_slots; i++) {
        if (bus->slots[i].router_id == router_id)
            return &bus->slots[i];
    }
    return NULL;
}

static media_peer_slot_t *bus_first_slot_kind(media_peer_bus_t *bus, media_peer_kind_t kind)
{
    int i;

    if (!bus)
        return NULL;
    for (i = 0; i < bus->n_slots; i++) {
        if (bus->slots[i].open && bus->slots[i].kind == kind)
            return &bus->slots[i];
    }
    return NULL;
}

const media_peer_slot_t *media_peer_bus_slot(const media_peer_bus_t *bus, int router_id)
{
    return bus_find_slot((media_peer_bus_t *)bus, router_id);
}

media_peer_slot_t *media_peer_bus_slot_mut(media_peer_bus_t *bus, int router_id)
{
    return bus_find_slot(bus, router_id);
}

peer_dmr_t *media_peer_bus_dmr(media_peer_bus_t *bus, int router_id)
{
    media_peer_slot_t *s;

    if (!bus)
        return NULL;
    if (router_id < 0)
        s = bus_first_slot_kind(bus, MEDIA_PEER_DMR);
    else
        s = bus_find_slot(bus, router_id);
    if (!s || s->kind != MEDIA_PEER_DMR || !s->open)
        return NULL;
    return &s->u.dmr;
}

peer_ysf_t *media_peer_bus_ysf(media_peer_bus_t *bus, int router_id)
{
    media_peer_slot_t *s;

    if (!bus)
        return NULL;
    if (router_id < 0)
        s = bus_first_slot_kind(bus, MEDIA_PEER_YSF);
    else
        s = bus_find_slot(bus, router_id);
    if (!s || s->kind != MEDIA_PEER_YSF || !s->open)
        return NULL;
    return &s->u.ysf;
}

peer_echolink_t *media_peer_bus_el(media_peer_bus_t *bus, int router_id)
{
    media_peer_slot_t *s;

    if (!bus)
        return NULL;
    if (router_id < 0)
        s = bus_first_slot_kind(bus, MEDIA_PEER_ECHOLINK);
    else
        s = bus_find_slot(bus, router_id);
    if (!s || s->kind != MEDIA_PEER_ECHOLINK || !s->open)
        return NULL;
    return &s->u.el;
}

peer_dmr_t *media_peer_bus_primary_dmr(media_peer_bus_t *bus)
{
    return media_peer_bus_dmr(bus, -1);
}

peer_ysf_t *media_peer_bus_primary_ysf(media_peer_bus_t *bus)
{
    return media_peer_bus_ysf(bus, -1);
}

peer_echolink_t *media_peer_bus_primary_el(media_peer_bus_t *bus)
{
    return media_peer_bus_el(bus, -1);
}

static int bus_open_slot(media_peer_slot_t *slot, const adn_bridge_config_t *cfg)
{
    const adn_bridge_peer_t *p;
    const peer_kind_plugin_t *plug;

    if (!cfg || slot->cfg_index < 0 || slot->cfg_index >= cfg->peer_count)
        return -1;
    p = &cfg->peers[slot->cfg_index];
    if (!p->type_set || !p->enabled)
        return -1;
    plug = peer_plugin_for_config_type(p->type);
    if (!plug || plug->router_kind != slot->kind)
        return -1;

    switch (slot->kind) {
    case MEDIA_PEER_DMR: {
        const adn_bridge_peer_dmr_t *d = &p->u.dmr;

        if (peer_dmr_open(&slot->u.dmr, d->host, d->port, d->callsign, d->dmrid, d->tg,
                          d->options, d->password, d->description, d->location) < 0)
            return -1;
        slot->clear_dynamic_tg = d->clear_dynamic_tg ? 1 : 0;
        break;
    }
    case MEDIA_PEER_YSF: {
        const adn_bridge_peer_ysf_t *y = &p->u.ysf;

        if (peer_ysf_open(&slot->u.ysf, y->host, y->port, y->callsign, (uint8_t)y->dgid) < 0)
            return -1;
        break;
    }
    case MEDIA_PEER_ECHOLINK: {
        if (peer_el_open(&slot->u.el, &p->u.el) < 0)
            return -1;
        break;
    }
    default:
        return -1;
    }
    slot->open = 1;
    return 0;
}

int media_peer_bus_open_all(media_peer_bus_t *bus, const adn_bridge_config_t *cfg)
{
    const media_router_t *r;
    int i, n = 0;

    if (!bus || !cfg || !bus->router)
        return -1;
    r = bus->router;
    bus->n_slots = 0;

    for (i = 0; i < r->n_peers; i++) {
        const media_router_peer_t *rp = &r->peers[i];
        media_peer_slot_t *slot;

        if (!rp->enabled || rp->cfg_index < 0)
            continue;
        if (bus->n_slots >= MEDIA_ROUTER_MAX_PEERS)
            return -1;
        slot = &bus->slots[bus->n_slots];
        memset(slot, 0, sizeof(*slot));
        slot->router_id = i;
        slot->cfg_index = rp->cfg_index;
        slot->kind = rp->kind;
        if (bus_open_slot(slot, cfg) != 0)
            return -1;
        bus->n_slots++;
        n++;
    }
    return n > 0 ? 0 : -1;
}

void media_peer_bus_close_all(media_peer_bus_t *bus)
{
    int i;

    if (!bus)
        return;
    for (i = 0; i < bus->n_slots; i++) {
        media_peer_slot_t *slot = &bus->slots[i];

        if (!slot->open)
            continue;
        switch (slot->kind) {
        case MEDIA_PEER_DMR:
            peer_dmr_close(&slot->u.dmr);
            break;
        case MEDIA_PEER_YSF:
            peer_ysf_close(&slot->u.ysf);
            break;
        case MEDIA_PEER_ECHOLINK:
            peer_el_close(&slot->u.el);
            break;
        default:
            break;
        }
        slot->open = 0;
    }
}

void media_peer_bus_sigint_all(media_peer_bus_t *bus)
{
    int i;

    if (!bus)
        return;
    for (i = 0; i < bus->n_slots; i++) {
        media_peer_slot_t *slot = &bus->slots[i];

        if (!slot->open)
            continue;
        switch (slot->kind) {
        case MEDIA_PEER_DMR:
            peer_dmr_on_sigint(&slot->u.dmr);
            break;
        case MEDIA_PEER_YSF:
            peer_ysf_on_sigint(&slot->u.ysf);
            break;
        case MEDIA_PEER_ECHOLINK:
            peer_el_on_sigint(&slot->u.el);
            break;
        default:
            break;
        }
    }
}

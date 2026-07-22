/*
 * Runtime peer instances — one slot per router peer (multi-dest fan-out).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_PEER_BUS_H
#define ADN_MEDIA_PEER_BUS_H

#include "config.h"
#include "media/router.h"
#include "peer_dmr.h"
#include "peer_echolink.h"
#include "peer_ysf.h"

typedef struct {
    int               router_id;
    int               cfg_index;
    media_peer_kind_t kind;
    int               open;
    union {
        peer_dmr_t        dmr;
        peer_ysf_t        ysf;
        peer_echolink_t   el;
    } u;
} media_peer_slot_t;

typedef struct {
    media_peer_slot_t slots[MEDIA_ROUTER_MAX_PEERS];
    int               n_slots;
    const media_router_t *router;
} media_peer_bus_t;

void media_peer_bus_init(media_peer_bus_t *bus, const media_router_t *router);

int media_peer_bus_open_all(media_peer_bus_t *bus, const adn_bridge_config_t *cfg);
void media_peer_bus_close_all(media_peer_bus_t *bus);
void media_peer_bus_sigint_all(media_peer_bus_t *bus);

const media_peer_slot_t *media_peer_bus_slot(const media_peer_bus_t *bus, int router_id);

peer_dmr_t *media_peer_bus_dmr(media_peer_bus_t *bus, int router_id);
peer_ysf_t *media_peer_bus_ysf(media_peer_bus_t *bus, int router_id);
peer_echolink_t *media_peer_bus_el(media_peer_bus_t *bus, int router_id);

peer_dmr_t *media_peer_bus_primary_dmr(media_peer_bus_t *bus);
peer_ysf_t *media_peer_bus_primary_ysf(media_peer_bus_t *bus);
peer_echolink_t *media_peer_bus_primary_el(media_peer_bus_t *bus);

#endif

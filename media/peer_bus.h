/*
 * Runtime peer instances — one slot per router peer (multi-dest fan-out).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_PEER_BUS_H
#define ADN_MEDIA_PEER_BUS_H

#include <stdint.h>
#include <time.h>

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
    /* Per-destination DMR TX framing (DMR-kind slots only) — wire-correct
     * multi-DMR fan-out needs a distinct seq/stream per destination
     * connection, not one shared across all of them. Reset by the core
     * pathway at call-begin (see media/core_ysf_dmr.c, media/core_echolink.c). */
    uint8_t           dmr_tx_seq;
    uint32_t          dmr_tx_stream_id;
    /* Per-destination connect-PTT (DMR-kind slots only): each DMR peer may
     * have its own TG and its own clear_dynamic_tg config, so each needs an
     * independent clear-then-activate sequence, not one shared TG fanned
     * out to every destination. clear_dynamic_tg is copied from config at
     * open time; the rest is session state advanced once per tick. */
    int               clear_dynamic_tg;
    int               dmr_was_connected;
    int               cp_active;
    int               cp_phase; /* 0=need VHEAD, 1=voice */
    int               cp_voice_frames;
    int               cp_tg;
    int               cp_clearing; /* 1 = current stage is the TG 4000 clear burst */
    struct timespec   cp_start;
    /* Per-source YSF relay framing (YSF-kind slots only, media/core_relay.c):
     * FICH fn/net_cnt cycling for a same-protocol YSF<->YSF relay call, kept
     * separate from media_core_t.ysf_cnt (the cross-kind ModeConv paths'
     * counter) so a relay call can run without disturbing those. */
    uint8_t           ysf_relay_cnt;
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

/* Mutable lookup for per-destination TX state (dmr_tx_seq/dmr_tx_stream_id). */
media_peer_slot_t *media_peer_bus_slot_mut(media_peer_bus_t *bus, int router_id);

peer_dmr_t *media_peer_bus_dmr(media_peer_bus_t *bus, int router_id);
peer_ysf_t *media_peer_bus_ysf(media_peer_bus_t *bus, int router_id);

peer_dmr_t *media_peer_bus_primary_dmr(media_peer_bus_t *bus);
peer_ysf_t *media_peer_bus_primary_ysf(media_peer_bus_t *bus);
peer_echolink_t *media_peer_bus_primary_el(media_peer_bus_t *bus);

#endif

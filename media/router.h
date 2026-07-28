/*
 * Media router — single ingress, fan-out to all other enabled peers.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_ROUTER_H
#define ADN_MEDIA_ROUTER_H

#include <stddef.h>

#define MEDIA_ROUTER_MAX_PEERS 16
#define MEDIA_ROUTER_CFG_NONE  (-1)

typedef enum {
    MEDIA_PEER_ECHOLINK = 0,
    MEDIA_PEER_DMR,
    MEDIA_PEER_YSF,
} media_peer_kind_t;

typedef struct {
    int               id;
    media_peer_kind_t kind;
    int               enabled;
    int               cfg_index; /* adn_bridge_config peers[] index, or MEDIA_ROUTER_CFG_NONE */
} media_router_peer_t;

typedef struct {
    media_router_peer_t peers[MEDIA_ROUTER_MAX_PEERS];
    int                 n_peers;
    int                 active_ingress; /* peer id, or -1 */
} media_router_t;

void media_router_init(media_router_t *r);

/* Register peer; returns peer id or -1 on table full. */
int media_router_add_peer(media_router_t *r, media_peer_kind_t kind);

/* Register with config slot and enabled flag (opt-out via enabled=0). */
int media_router_add_peer_cfg(media_router_t *r, media_peer_kind_t kind,
                               int cfg_index, int enabled);

int media_router_peer_count(const media_router_t *r);

/* First enabled router slot for kind, or -1. */
int media_router_find_first(const media_router_t *r, media_peer_kind_t kind);
int media_router_active_ingress(const media_router_t *r);

/* 1 = voice from src may enter; 0 = drop (another peer is active ingress). */
int media_router_ingress_allowed(const media_router_t *r, int src_id);

void media_router_ingress_begin(media_router_t *r, int src_id);
void media_router_ingress_end(media_router_t *r, int src_id);

typedef int (*media_router_visit_fn)(int dst_id, media_peer_kind_t kind, void *ctx);

/* Invoke fn for each enabled peer other than src_id. Returns visit count. */
int media_router_fanout(const media_router_t *r, int src_id,
                         media_router_visit_fn fn, void *ctx);

#endif

/*
 * Media router — single ingress, fan-out to all other enabled peers.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/router.h"

#include <string.h>

void media_router_init(media_router_t *r)
{
    memset(r, 0, sizeof(*r));
    r->active_ingress = -1;
}

int media_router_add_peer_cfg(media_router_t *r, media_peer_kind_t kind,
                               int cfg_index, int enabled)
{
    media_router_peer_t *p;

    if (!r || r->n_peers >= MEDIA_ROUTER_MAX_PEERS)
        return -1;
    p = &r->peers[r->n_peers];
    p->id = r->n_peers;
    p->kind = kind;
    p->enabled = enabled ? 1 : 0;
    p->cfg_index = cfg_index;
    r->n_peers++;
    return p->id;
}

int media_router_add_peer(media_router_t *r, media_peer_kind_t kind)
{
    return media_router_add_peer_cfg(r, kind, MEDIA_ROUTER_CFG_NONE, 1);
}

int media_router_find_first(const media_router_t *r, media_peer_kind_t kind)
{
    int i;

    if (!r)
        return -1;
    for (i = 0; i < r->n_peers; i++) {
        if (r->peers[i].enabled && r->peers[i].kind == kind)
            return i;
    }
    return -1;
}

int media_router_peer_count(const media_router_t *r)
{
    return r ? r->n_peers : 0;
}

int media_router_active_ingress(const media_router_t *r)
{
    return r ? r->active_ingress : -1;
}

int media_router_ingress_allowed(const media_router_t *r, int src_id)
{
    if (!r || src_id < 0 || src_id >= r->n_peers)
        return 0;
    if (!r->peers[src_id].enabled)
        return 0;
    return r->active_ingress < 0 || r->active_ingress == src_id;
}

void media_router_ingress_begin(media_router_t *r, int src_id)
{
    if (!r || src_id < 0 || src_id >= r->n_peers)
        return;
    if (!media_router_ingress_allowed(r, src_id))
        return;
    r->active_ingress = src_id;
}

void media_router_ingress_end(media_router_t *r, int src_id)
{
    if (!r)
        return;
    if (src_id < 0) {
        r->active_ingress = -1;
        return;
    }
    if (r->active_ingress == src_id)
        r->active_ingress = -1;
}

int media_router_fanout(const media_router_t *r, int src_id,
                          media_router_visit_fn fn, void *ctx)
{
    int i, n = 0;

    if (!r || !fn || src_id < 0 || src_id >= r->n_peers)
        return 0;
    for (i = 0; i < r->n_peers; i++) {
        if (i == src_id || !r->peers[i].enabled)
            continue;
        if (fn(i, r->peers[i].kind, ctx) == 0)
            n++;
    }
    return n;
}

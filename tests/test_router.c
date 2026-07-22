/*
 * Unit tests for media/router.c
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/router.h"

#include <stdio.h>
#include <stdlib.h>

static int fanout_count;
static int fanout_dst_kind;

static int count_dest(int dst_id, media_peer_kind_t kind, void *ctx)
{
    (void)dst_id;
    (void)ctx;
    fanout_count++;
    fanout_dst_kind = (int)kind;
    return 0;
}

int main(void)
{
    media_router_t r;
    int el, dmr, ysf;

    media_router_init(&r);
    if (r.active_ingress != -1)
        return 1;

    el = media_router_add_peer(&r, MEDIA_PEER_ECHOLINK);
    dmr = media_router_add_peer(&r, MEDIA_PEER_DMR);
    ysf = media_router_add_peer(&r, MEDIA_PEER_YSF);
    if (el != 0 || dmr != 1 || ysf != 2 || media_router_peer_count(&r) != 3)
        return 2;

    if (!media_router_ingress_allowed(&r, el))
        return 3;
    media_router_ingress_begin(&r, el);
    if (media_router_active_ingress(&r) != el)
        return 4;
    if (media_router_ingress_allowed(&r, dmr))
        return 5;
    media_router_ingress_end(&r, el);
    if (media_router_active_ingress(&r) != -1)
        return 6;
    if (!media_router_ingress_allowed(&r, dmr))
        return 7;

    fanout_count = 0;
    fanout_dst_kind = -1;
    if (media_router_fanout(&r, el, count_dest, NULL) != 2)
        return 8;
    if (fanout_count != 2)
        return 9;

    fanout_count = 0;
    if (media_router_fanout(&r, dmr, count_dest, NULL) != 2)
        return 10;
    if (fanout_dst_kind != MEDIA_PEER_YSF && fanout_dst_kind != MEDIA_PEER_ECHOLINK)
        return 11;

    media_router_set_peer_enabled(&r, ysf, 0);
    fanout_count = 0;
    if (media_router_fanout(&r, dmr, count_dest, NULL) != 1)
        return 12;
    if (fanout_count != 1 || fanout_dst_kind != MEDIA_PEER_ECHOLINK)
        return 13;

    if (media_router_find_first(&r, MEDIA_PEER_YSF) != -1)
        return 14;
    if (media_router_find_first(&r, MEDIA_PEER_DMR) != dmr)
        return 15;
    if (media_router_peer_cfg_index(&r, el) != MEDIA_ROUTER_CFG_NONE)
        return 16;

    printf("test_router: ok\n");
    return 0;
}

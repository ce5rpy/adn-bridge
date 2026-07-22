/*
 * Unit checks for media_core stub (Fase 1: contracts, no session logic yet).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "media/core.h"
#include "media/router.h"

static void test_init_defaults(void)
{
    media_core_t core;

    media_core_init(&core);
    assert(core.ingress_router_id == -1);
    assert(core.phase == MEDIA_CALL_IDLE);
    assert(core.dmr_slot_bit == 0x80);
    assert(core.use_vocoder == 0);
}

static void test_bind_sets_use_vocoder(void)
{
    media_core_t core;
    media_router_t r;
    media_codec_plan_t plan;

    media_router_init(&r);
    media_router_add_peer_cfg(&r, MEDIA_PEER_ECHOLINK, 0, 1);
    media_router_add_peer_cfg(&r, MEDIA_PEER_DMR, 1, 1);
    media_codec_plan_build(&r, &plan);

    media_core_init(&core);
    media_core_bind(&core, &r, NULL, &plan, NULL);
    assert(core.use_vocoder == 1);
}

static void test_ingress_drops_when_blocked(void)
{
    media_core_t core;
    media_router_t r;
    media_bus_frame_t frame;
    int dmr, ysf;

    media_router_init(&r);
    dmr = media_router_add_peer(&r, MEDIA_PEER_DMR);
    ysf = media_router_add_peer(&r, MEDIA_PEER_YSF);

    media_core_init(&core);
    media_core_bind(&core, &r, NULL, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_VOICE;
    frame.codec = CODEC_DMR_AMBE;

    /* dmr becomes active ingress; ysf must be blocked until dmr releases it. */
    media_router_ingress_begin(&r, dmr);
    assert(media_router_ingress_allowed(&r, ysf) == 0);
    media_core_ingress(&core, ysf, &frame); /* must not assert / must be a no-op */
    assert(media_router_ingress_allowed(&r, dmr) == 1);

    media_router_ingress_end(&r, dmr);
    assert(media_router_ingress_allowed(&r, ysf) == 1);
}

int main(void)
{
    test_init_defaults();
    test_bind_sets_use_vocoder();
    test_ingress_drops_when_blocked();
    printf("test_media_core: ok\n");
    return 0;
}

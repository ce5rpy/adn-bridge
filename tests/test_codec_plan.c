/*
 * Unit checks for media codec plan (vocoder / ModeConv flags).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "media/codec_plan.h"
#include "media/log_flow.h"
#include "media/router.h"

static void test_ysf_dmr_no_vocoder(void)
{
    media_router_t r;
    media_codec_plan_t plan;

    media_router_init(&r);
    media_router_add_peer_cfg(&r, MEDIA_PEER_YSF, 0, 1);
    media_router_add_peer_cfg(&r, MEDIA_PEER_DMR, 1, 1);
    media_codec_plan_build(&r, &plan);

    assert(plan.needs_modeconv == 1);
    assert(plan.needs_vocoder == 0);
    assert(plan.has_ysf && plan.has_dmr);
    assert(!plan.has_el);
}

static void test_el_dmr_needs_vocoder(void)
{
    media_router_t r;
    media_codec_plan_t plan;

    media_router_init(&r);
    media_router_add_peer_cfg(&r, MEDIA_PEER_ECHOLINK, 0, 1);
    media_router_add_peer_cfg(&r, MEDIA_PEER_DMR, 1, 1);
    media_codec_plan_build(&r, &plan);

    assert(plan.needs_vocoder == 1);
    assert(plan.has_el && plan.has_dmr);
}

static void test_el_el_relay_no_vocoder(void)
{
    media_router_t r;
    media_codec_plan_t plan;

    media_router_init(&r);
    media_router_add_peer_cfg(&r, MEDIA_PEER_ECHOLINK, 0, 1);
    media_router_add_peer_cfg(&r, MEDIA_PEER_ECHOLINK, 1, 1);
    media_codec_plan_build(&r, &plan);

    assert(plan.needs_vocoder == 0);
    assert(plan.needs_modeconv == 0);
    assert(plan.has_el);
}

static void test_flow_label(void)
{
    assert(strcmp(media_flow_label(MEDIA_PEER_ECHOLINK, MEDIA_PEER_DMR), "echolink->dmr") == 0);
    assert(strcmp(media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR), "ysf->dmr") == 0);
}

int main(void)
{
    test_ysf_dmr_no_vocoder();
    test_el_dmr_needs_vocoder();
    test_el_el_relay_no_vocoder();
    test_flow_label();
    printf("test_codec_plan: ok\n");
    return 0;
}

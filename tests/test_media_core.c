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
#include "media/peer_bus.h"
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
    media_core_bind(&core, ADN_BRIDGE_LAYOUT_EL_DMR, &r, NULL, &plan, NULL);
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
    media_core_bind(&core, ADN_BRIDGE_LAYOUT_YSF_DMR, &r, NULL, NULL, NULL);

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

static void setup_ysf_dmr_bus(media_peer_bus_t *bus, media_router_t *r, int dmr_id, int ysf_id)
{
    memset(bus, 0, sizeof(*bus));
    bus->router = r;
    bus->n_slots = 2;

    bus->slots[0].router_id = dmr_id;
    bus->slots[0].kind = MEDIA_PEER_DMR;
    bus->slots[0].open = 1;
    bus->slots[0].u.dmr.sock = -1;
    bus->slots[0].u.dmr.dmrid = 7141001;
    bus->slots[0].u.dmr.tg = 7141;
    memset(bus->slots[0].u.dmr.callsign, ' ', sizeof(bus->slots[0].u.dmr.callsign));

    bus->slots[1].router_id = ysf_id;
    bus->slots[1].kind = MEDIA_PEER_YSF;
    bus->slots[1].open = 1;
    bus->slots[1].u.ysf.sock = -1;
    memset(bus->slots[1].u.ysf.callsign, ' ', sizeof(bus->slots[1].u.ysf.callsign));
}

/* DMR call-begin must take router ingress and carry stream_id/seq from the
 * frame into core state (media/core_ysf_dmr.c: core_begin_dmr_to_ysf). */
static void test_dmr_call_begin_takes_ingress(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int dmr_id, ysf_id;

    media_router_init(&r);
    dmr_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    ysf_id = media_router_add_peer(&r, MEDIA_PEER_YSF);
    setup_ysf_dmr_bus(&bus, &r, dmr_id, ysf_id);

    media_core_init(&core);
    media_core_bind(&core, ADN_BRIDGE_LAYOUT_YSF_DMR, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = 42;
    frame.meta.talker_id = 7141001;
    frame.wire_seq = 5;
    memset(frame.meta.netcall.net_src, ' ', 10);
    memcpy(frame.meta.netcall.net_src, "TESTCALL", 8);

    media_core_ingress(&core, dmr_id, &frame);

    assert(core.phase == MEDIA_CALL_TX_TO_PEER);
    assert(media_router_active_ingress(&r) == dmr_id);
    assert(core.call.stream_id == 42);
    assert(core.dmr_seq == 5);
    assert(core.call.talker_id == 7141001);
}

/*
 * Faithful bug-for-bug parity with bridge.c: both directions share a single
 * call phase (like adn_bridge_t.call_active), so a YSF call-begin while a DMR
 * call is already active does NOT reset/steal — it is treated as a
 * continuation (feeds modeconv_put_ysf_header, does not touch router state).
 * This is v0.3.1's actual behaviour, not a design choice of this refactor —
 * see docs-priv/media-bus-dumb-modes-plan.md correction #4 / risk table.
 */
static void test_shared_phase_no_cross_peer_preempt(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int dmr_id, ysf_id;

    media_router_init(&r);
    dmr_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    ysf_id = media_router_add_peer(&r, MEDIA_PEER_YSF);
    setup_ysf_dmr_bus(&bus, &r, dmr_id, ysf_id);

    media_core_init(&core);
    media_core_bind(&core, ADN_BRIDGE_LAYOUT_YSF_DMR, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = 1;
    frame.meta.talker_id = 7141001;
    media_core_ingress(&core, dmr_id, &frame);
    assert(media_router_active_ingress(&r) == dmr_id);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    frame.meta.talker_id = 99;
    media_core_ingress(&core, ysf_id, &frame);

    /* Router ingress untouched — DMR is still the "active" peer of record. */
    assert(media_router_active_ingress(&r) == dmr_id);
    assert(core.phase == MEDIA_CALL_TX_TO_PEER);
}

/* DMR call-begin in an EL<->DMR layout must take RX_FROM_PEER phase (the
 * "peer -> EL" leg) — media/core_echolink.c: core_el_dmr_ingress_dmr. */
static void test_el_dmr_call_begin_takes_rx_phase(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int el_id, dmr_id;

    media_router_init(&r);
    el_id = media_router_add_peer(&r, MEDIA_PEER_ECHOLINK);
    dmr_id = media_router_add_peer(&r, MEDIA_PEER_DMR);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 2;
    bus.slots[0].router_id = el_id;
    bus.slots[0].kind = MEDIA_PEER_ECHOLINK;
    bus.slots[0].open = 1;
    bus.slots[1].router_id = dmr_id;
    bus.slots[1].kind = MEDIA_PEER_DMR;
    bus.slots[1].open = 1;
    bus.slots[1].u.dmr.sock = -1;
    bus.slots[1].u.dmr.dmrid = 7141001;
    bus.slots[1].u.dmr.tg = 7141;

    media_core_init(&core);
    media_core_bind(&core, ADN_BRIDGE_LAYOUT_EL_DMR, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = 7;

    media_core_ingress(&core, dmr_id, &frame);

    assert(core.phase == MEDIA_CALL_RX_FROM_PEER);
    assert(core.dmr_rx_stream_id == 7);
    assert(media_router_active_ingress(&r) == dmr_id);
}

/* YSF call-begin in an EL<->YSF layout must take RX_FROM_PEER phase —
 * media/core_echolink.c: core_el_ysf_ingress_ysf. */
static void test_el_ysf_call_begin_takes_rx_phase(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int el_id, ysf_id;

    media_router_init(&r);
    el_id = media_router_add_peer(&r, MEDIA_PEER_ECHOLINK);
    ysf_id = media_router_add_peer(&r, MEDIA_PEER_YSF);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 2;
    bus.slots[0].router_id = el_id;
    bus.slots[0].kind = MEDIA_PEER_ECHOLINK;
    bus.slots[0].open = 1;
    bus.slots[1].router_id = ysf_id;
    bus.slots[1].kind = MEDIA_PEER_YSF;
    bus.slots[1].open = 1;
    bus.slots[1].u.ysf.sock = -1;

    media_core_init(&core);
    media_core_bind(&core, ADN_BRIDGE_LAYOUT_EL_YSF, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    memset(frame.meta.netcall.net_src, ' ', 10);
    memcpy(frame.meta.netcall.net_src, "N0CALL", 6);

    media_core_ingress(&core, ysf_id, &frame);

    assert(core.phase == MEDIA_CALL_RX_FROM_PEER);
    assert(media_router_active_ingress(&r) == ysf_id);
    assert(memcmp(core.call.netcall.net_src, "N0CALL", 6) == 0);
}

int main(void)
{
    test_init_defaults();
    test_bind_sets_use_vocoder();
    test_ingress_drops_when_blocked();
    test_dmr_call_begin_takes_ingress();
    test_shared_phase_no_cross_peer_preempt();
    test_el_dmr_call_begin_takes_rx_phase();
    test_el_ysf_call_begin_takes_rx_phase();
    printf("test_media_core: ok\n");
    return 0;
}

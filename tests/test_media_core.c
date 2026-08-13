/*
 * Unit checks for media_core stub (Fase 1: contracts, no session logic yet).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>

#include "media/core.h"
#include "media/core_echolink.h"
#include "media/core_ysf_dmr.h"
#include "media/peer_bus.h"
#include "media/router.h"
#include "session/dmr_tx.h"
#include "session/dmr_wire.h"
#include "ysf_fich.h"

static void test_init_defaults(void)
{
    media_core_t core;

    media_core_init(&core);
    assert(core.leg_ysf_dmr.phase == MEDIA_CALL_IDLE);
    assert(core.leg_el_rx.phase == MEDIA_CALL_IDLE);
    assert(core.leg_el_tx_dmr.phase == MEDIA_CALL_IDLE);
    assert(core.leg_el_tx_ysf.phase == MEDIA_CALL_IDLE);
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
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = 42;
    frame.meta.talker_id = 7141001;
    frame.wire_seq = 5;
    memset(frame.meta.netcall.net_src, ' ', 10);
    memcpy(frame.meta.netcall.net_src, "TESTCALL", 8);

    media_core_ingress(&core, dmr_id, &frame);

    assert(core.leg_ysf_dmr.phase == MEDIA_CALL_TX_TO_PEER);
    assert(media_router_active_ingress(&r) == dmr_id);
    assert(core.leg_ysf_dmr.call.stream_id == 42);
    assert(core.leg_ysf_dmr.dmr_seq == 5);
    assert(core.leg_ysf_dmr.call.talker_id == 7141001);
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
    media_core_bind(&core, &r, &bus, NULL, NULL);

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
    assert(core.leg_ysf_dmr.phase == MEDIA_CALL_TX_TO_PEER);
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
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = 7;

    media_core_ingress(&core, dmr_id, &frame);

    assert(core.leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER);
    assert(core.leg_el_rx.rx_src_kind == MEDIA_PEER_DMR);
    assert(core.leg_el_rx.dmr_rx_stream_id == 7);
    assert(media_router_active_ingress(&r) == dmr_id);
}

/* A connect-PTT burst on a DMR slot must pause EL->DMR AMBE encoding too
 * (core_el_process_el_audio), not just the ModeConv drain
 * (core_el_dmr_pace_tx) — otherwise core->mc_el_dmr's ring buffer fills while
 * draining is paused, desyncing ModeConv's internal frame counter from the
 * buffer's real contents (a real bug hit in production, inherited unchanged
 * from v0.3.1's bridge_el.c: the capture path lacked this gate that pace_tx
 * always had). */
static void test_el_dmr_process_el_audio_pauses_during_connect_ptt(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_codec_plan_t plan;
    media_peer_slot_t *dmr_slot;
    int el_id, dmr_id;
    int i;

    media_router_init(&r);
    el_id = media_router_add_peer(&r, MEDIA_PEER_ECHOLINK);
    dmr_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    media_codec_plan_build(&r, &plan);

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
    bus.slots[1].u.dmr.status = PEER_DMR_CONNECTED;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, &plan, NULL);
    assert(core.use_vocoder == 1); /* EL+DMR always needs the PCM hub */

    /* Pretend 160 samples of EL RTP audio already arrived. */
    for (i = 0; i < 160; i++)
        bus.slots[0].u.el.pcm_in[i] = 100;
    bus.slots[0].u.el.pcm_in_count = 160;

    dmr_slot = media_peer_bus_slot_mut(&bus, dmr_id);
    dmr_slot->cp_active = 1; /* connect-PTT burst in progress on this DMR peer */

    core_el_process_el_audio(&core);

    /* Paused: nothing drained from EL's own jitter buffer. */
    assert(bus.slots[0].u.el.pcm_in_count == 160);
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
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    memset(frame.meta.netcall.net_src, ' ', 10);
    memcpy(frame.meta.netcall.net_src, "N0CALL", 6);

    media_core_ingress(&core, ysf_id, &frame);

    assert(core.leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER);
    assert(core.leg_el_rx.rx_src_kind == MEDIA_PEER_YSF);
    assert(media_router_active_ingress(&r) == ysf_id);
    assert(memcmp(core.leg_el_rx.call.netcall.net_src, "N0CALL", 6) == 0);
}

/* Fase 5: fan-out to >1 DMR destination must give each connection its own
 * seq/stream, not one shared across all of them (media_peer_slot_t.dmr_tx_*,
 * reset by core_reset_dmr_tx_slots in core_ysf_dmr.c/core_echolink.c). */
static void test_multi_dmr_fanout_gets_distinct_tx_state(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int ysf_id, dmr1_id, dmr2_id;
    media_peer_slot_t *s1, *s2;

    media_router_init(&r);
    ysf_id = media_router_add_peer(&r, MEDIA_PEER_YSF);
    dmr1_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    dmr2_id = media_router_add_peer(&r, MEDIA_PEER_DMR);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 3;
    bus.slots[0].router_id = ysf_id;
    bus.slots[0].kind = MEDIA_PEER_YSF;
    bus.slots[0].open = 1;
    bus.slots[0].u.ysf.sock = -1;
    bus.slots[1].router_id = dmr1_id;
    bus.slots[1].kind = MEDIA_PEER_DMR;
    bus.slots[1].open = 1;
    bus.slots[1].u.dmr.sock = -1;
    bus.slots[1].u.dmr.dmrid = 7141001;
    bus.slots[1].u.dmr.tg = 7141;
    bus.slots[2].router_id = dmr2_id;
    bus.slots[2].kind = MEDIA_PEER_DMR;
    bus.slots[2].open = 1;
    bus.slots[2].u.dmr.sock = -1;
    bus.slots[2].u.dmr.dmrid = 7141002;
    bus.slots[2].u.dmr.tg = 7142;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    frame.meta.talker_id = 7141001;
    memset(frame.meta.netcall.net_src, ' ', 10);
    memcpy(frame.meta.netcall.net_src, "TESTCALL", 8);

    media_core_ingress(&core, ysf_id, &frame);

    s1 = media_peer_bus_slot_mut(&bus, dmr1_id);
    s2 = media_peer_bus_slot_mut(&bus, dmr2_id);
    assert(s1 && s2);
    assert(s1->dmr_tx_seq == 0 && s2->dmr_tx_seq == 0);
    assert(s1->dmr_tx_stream_id != 0 && s2->dmr_tx_stream_id != 0);
    /* Each destination gets its own bridge_new_stream_id() call — sharing
     * one field across destinations is exactly the bug Fase 5 fixes. */
    assert(s1->dmr_tx_stream_id != s2->dmr_tx_stream_id);
}

/* Fase 7: pure same-kind relay (the "diagonal") — 2 DMR peers, no YSF/EL at
 * all. Config validation used to reject this outright ("unsupported peer
 * mix"); media/core_relay.c now relays it wire-only. Relay never touches
 * core.leg_*.phase (see media/core_relay.c) — only the router's shared
 * active_ingress marks who's on the air. */
static void test_dmr_relay_two_peers(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int dmr1_id, dmr2_id;
    media_peer_slot_t *s2;

    media_router_init(&r);
    dmr1_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    dmr2_id = media_router_add_peer(&r, MEDIA_PEER_DMR);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 2;
    bus.slots[0].router_id = dmr1_id;
    bus.slots[0].kind = MEDIA_PEER_DMR;
    bus.slots[0].open = 1;
    bus.slots[0].u.dmr.sock = -1;
    bus.slots[0].u.dmr.dmrid = 7141001;
    bus.slots[0].u.dmr.tg = 7141;
    bus.slots[1].router_id = dmr2_id;
    bus.slots[1].kind = MEDIA_PEER_DMR;
    bus.slots[1].open = 1;
    bus.slots[1].u.dmr.sock = -1;
    bus.slots[1].u.dmr.dmrid = 7141002;
    bus.slots[1].u.dmr.tg = 7142;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.talker_id = 7141001;

    media_core_ingress(&core, dmr1_id, &frame);

    assert(media_router_active_ingress(&r) == dmr1_id);
    /* Relay is phase-less by design — neither leg is touched. */
    assert(core.leg_ysf_dmr.phase == MEDIA_CALL_IDLE);
    assert(core.leg_el_rx.phase == MEDIA_CALL_IDLE);
    assert(core.leg_el_tx_dmr.phase == MEDIA_CALL_IDLE);
    assert(core.leg_el_tx_ysf.phase == MEDIA_CALL_IDLE);
    s2 = media_peer_bus_slot_mut(&bus, dmr2_id);
    assert(s2 && s2->dmr_tx_stream_id != 0);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_END;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.talker_id = 7141001;
    media_core_ingress(&core, dmr1_id, &frame);

    assert(media_router_active_ingress(&r) == -1);
}

/* Fase 7: a 3+-kind bus can fire relay, direct (ModeConv) and pcm (vocoder)
 * for the same source frame at once — verifies the independent leg-state
 * split (media_leg_ysf_dmr_t/media_leg_el_t) actually prevents the two
 * cross-kind pathways from clobbering each other's phase. */
static void test_three_kind_bus_relay_direct_pcm_concurrent(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int dmr1_id, dmr2_id, ysf_id, el_id;
    media_peer_slot_t *s2;

    media_router_init(&r);
    dmr1_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    dmr2_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    ysf_id = media_router_add_peer(&r, MEDIA_PEER_YSF);
    el_id = media_router_add_peer(&r, MEDIA_PEER_ECHOLINK);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 4;
    bus.slots[0].router_id = dmr1_id;
    bus.slots[0].kind = MEDIA_PEER_DMR;
    bus.slots[0].open = 1;
    bus.slots[0].u.dmr.sock = -1;
    bus.slots[0].u.dmr.dmrid = 7141001;
    bus.slots[0].u.dmr.tg = 7141;
    bus.slots[1].router_id = dmr2_id;
    bus.slots[1].kind = MEDIA_PEER_DMR;
    bus.slots[1].open = 1;
    bus.slots[1].u.dmr.sock = -1;
    bus.slots[1].u.dmr.dmrid = 7141002;
    bus.slots[1].u.dmr.tg = 7142;
    bus.slots[2].router_id = ysf_id;
    bus.slots[2].kind = MEDIA_PEER_YSF;
    bus.slots[2].open = 1;
    bus.slots[2].u.ysf.sock = -1;
    bus.slots[3].router_id = el_id;
    bus.slots[3].kind = MEDIA_PEER_ECHOLINK;
    bus.slots[3].open = 1;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_DMR_AMBE;
    frame.meta.stream_id = 55;
    frame.meta.talker_id = 7141001;

    media_core_ingress(&core, dmr1_id, &frame);

    assert(media_router_active_ingress(&r) == dmr1_id);
    /* direct (DMR->YSF via ModeConv) fired */
    assert(core.leg_ysf_dmr.phase == MEDIA_CALL_TX_TO_PEER);
    /* pcm (DMR->EL via vocoder) fired too, same frame — this is exactly the
     * case that used to corrupt when both legs shared one phase field. */
    assert(core.leg_el_rx.phase == MEDIA_CALL_RX_FROM_PEER);
    assert(core.leg_el_rx.rx_src_kind == MEDIA_PEER_DMR);
    /* relay (DMR->DMR) fired as well: dmr2's per-slot TX state was reset. */
    s2 = media_peer_bus_slot_mut(&bus, dmr2_id);
    assert(s2 && s2->dmr_tx_stream_id != 0);
}

static void rewind_ms(struct timespec *ts, long ms)
{
    ts->tv_sec -= ms / 1000;
    ts->tv_nsec -= (ms % 1000) * 1000000L;
    if (ts->tv_nsec < 0) {
        ts->tv_nsec += 1000000000L;
        ts->tv_sec -= 1;
    }
}

static int recv_nb(int fd, uint8_t *buf, size_t cap, int timeout_ms)
{
    struct pollfd pfd;

    pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return -1;
    return (int)recv(fd, buf, cap, 0);
}

/* Discard any packets already queued (e.g. a just-finished PTT's trailing
 * VTERM) so a later "nothing sent" check isn't fooled by a stale read. */
static void drain_all(int fd)
{
    uint8_t buf[64];

    while (recv_nb(fd, buf, sizeof(buf), 0) >= 0)
        ;
}

/* Fase 8: EchoLink mic audio must reach a DMR peer AND a YSF peer
 * concurrently in a DMR+YSF+EchoLink bus. Before this fix, both pathways
 * drained the same PCM ring buffer AND shared one modeconv instance
 * (core->mc_el) plus one phase field -- whichever ran first (always DMR,
 * by call order) silently starved/clobbered the other every tick. No real
 * vocoder runs in this test binary, so this bypasses the capture stage and
 * queues AMBE directly into each pathway's own modeconv instance, exactly
 * what core_el_process_el_audio would have done, then verifies one
 * core_el_tick drains BOTH to real wire bytes on independent sockets. */
static void test_el_tx_fanout_dmr_and_ysf_concurrent(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_peer_slot_t *dmr_slot, *ysf_slot;
    int el_id, dmr_id, ysf_id, n;
    int fake_dmr_fd, fake_ysf_fd;
    struct sockaddr_in dmr_addr, ysf_addr;
    socklen_t alen;
    uint8_t buf[160];
    uint8_t ambe[7];

    memset(ambe, 0x42, sizeof(ambe));

    fake_dmr_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fake_dmr_fd >= 0);
    memset(&dmr_addr, 0, sizeof(dmr_addr));
    dmr_addr.sin_family = AF_INET;
    dmr_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fake_dmr_fd, (struct sockaddr *)&dmr_addr, sizeof(dmr_addr)) == 0);
    alen = sizeof(dmr_addr);
    assert(getsockname(fake_dmr_fd, (struct sockaddr *)&dmr_addr, &alen) == 0);

    fake_ysf_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fake_ysf_fd >= 0);
    memset(&ysf_addr, 0, sizeof(ysf_addr));
    ysf_addr.sin_family = AF_INET;
    ysf_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fake_ysf_fd, (struct sockaddr *)&ysf_addr, sizeof(ysf_addr)) == 0);
    alen = sizeof(ysf_addr);
    assert(getsockname(fake_ysf_fd, (struct sockaddr *)&ysf_addr, &alen) == 0);

    media_router_init(&r);
    el_id = media_router_add_peer(&r, MEDIA_PEER_ECHOLINK);
    dmr_id = media_router_add_peer(&r, MEDIA_PEER_DMR);
    ysf_id = media_router_add_peer(&r, MEDIA_PEER_YSF);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 3;
    bus.slots[0].router_id = el_id;
    bus.slots[0].kind = MEDIA_PEER_ECHOLINK;
    bus.slots[0].open = 1;

    dmr_slot = &bus.slots[1];
    dmr_slot->router_id = dmr_id;
    dmr_slot->kind = MEDIA_PEER_DMR;
    dmr_slot->open = 1;
    dmr_slot->u.dmr.sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(dmr_slot->u.dmr.sock >= 0);
    dmr_slot->u.dmr.dmrid = 7141001;
    dmr_slot->u.dmr.tg = 7141;
    dmr_slot->u.dmr.peer.sin_family = AF_INET;
    dmr_slot->u.dmr.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dmr_slot->u.dmr.peer.sin_port = dmr_addr.sin_port;
    dmr_slot->u.dmr.status = PEER_DMR_CONNECTED;

    ysf_slot = &bus.slots[2];
    ysf_slot->router_id = ysf_id;
    ysf_slot->kind = MEDIA_PEER_YSF;
    ysf_slot->open = 1;
    ysf_slot->u.ysf.sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(ysf_slot->u.ysf.sock >= 0);
    memset(ysf_slot->u.ysf.callsign, ' ', sizeof(ysf_slot->u.ysf.callsign));
    ysf_slot->u.ysf.peer.sin_family = AF_INET;
    ysf_slot->u.ysf.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ysf_slot->u.ysf.peer.sin_port = ysf_addr.sin_port;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);
    /* The split that fixes the starvation bug: two genuinely separate
     * modeconv instances, not one shared between DMR-TX and YSF-TX. */
    assert(core.mc_el_dmr != NULL && core.mc_el != NULL && core.mc_el_dmr != core.mc_el);
    clock_gettime(CLOCK_MONOTONIC, &core.leg_el_capture.last_el_speech);

    /* Simulate what core_el_process_el_audio would have done once each
     * pathway is already mid-call: queue AMBE directly, bypassing the
     * vocoder (none running in this test binary). */
    core.leg_el_tx_dmr.phase = MEDIA_CALL_TX_TO_PEER;
    core.leg_el_tx_dmr.call.talker_id = 7300391;
    memcpy(core.leg_el_tx_dmr.call.netcall.net_src, "TESTCALL  ", 10);
    modeconv_reset(core.mc_el_dmr);
    modeconv_put_ambe7(core.mc_el_dmr, ambe);
    modeconv_put_ambe7(core.mc_el_dmr, ambe);
    modeconv_put_ambe7(core.mc_el_dmr, ambe);

    core.leg_el_tx_ysf.phase = MEDIA_CALL_TX_TO_PEER;
    memcpy(core.leg_el_tx_ysf.call.netcall.net_src, "TESTCALL  ", 10);
    modeconv_reset(core.mc_el);
    modeconv_put_dmr_header(core.mc_el);

    core_el_tick(&core);

    n = recv_nb(fake_dmr_fd, buf, sizeof(buf), 200);
    assert(n == 55 && memcmp(buf, "DMRD", 4) == 0);

    n = recv_nb(fake_ysf_fd, buf, sizeof(buf), 200);
    assert(n == 155 && memcmp(buf, "YSFD", 4) == 0);

    close(dmr_slot->u.dmr.sock);
    close(ysf_slot->u.ysf.sock);
    close(fake_dmr_fd);
    close(fake_ysf_fd);
}

/* A destination's embedded LC (encoded on n=0, read back on n=1..5 across
 * separate dmr_tx_send() calls) must not change depending on what other
 * destinations' calls run in between. */
static void test_dmr_tx_embedded_lc_independent_per_destination(void)
{
    peer_dmr_t dmrA, dmrB;
    dmr_tx_args_t argsA, argsB;
    uint8_t seqA, seqB;
    struct timespec last_txA, last_txB;
    bool emb_rawA[128], emb_rawB[128];
    int fake_fdA, fake_fdB;
    struct sockaddr_in addrA, addrB;
    socklen_t alen;
    uint8_t bufA[64], bufB[64], buf_isolated[64];
    uint8_t voice[33];

    fake_fdA = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fake_fdA >= 0);
    memset(&addrA, 0, sizeof(addrA));
    addrA.sin_family = AF_INET;
    addrA.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fake_fdA, (struct sockaddr *)&addrA, sizeof(addrA)) == 0);
    alen = sizeof(addrA);
    assert(getsockname(fake_fdA, (struct sockaddr *)&addrA, &alen) == 0);

    fake_fdB = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fake_fdB >= 0);
    memset(&addrB, 0, sizeof(addrB));
    addrB.sin_family = AF_INET;
    addrB.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fake_fdB, (struct sockaddr *)&addrB, sizeof(addrB)) == 0);
    alen = sizeof(addrB);
    assert(getsockname(fake_fdB, (struct sockaddr *)&addrB, &alen) == 0);

    memset(&dmrA, 0, sizeof(dmrA));
    dmrA.sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(dmrA.sock >= 0);
    dmrA.status = PEER_DMR_CONNECTED;
    dmrA.peer.sin_family = AF_INET;
    dmrA.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dmrA.peer.sin_port = addrA.sin_port;

    memset(&dmrB, 0, sizeof(dmrB));
    dmrB.sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(dmrB.sock >= 0);
    dmrB.status = PEER_DMR_CONNECTED;
    dmrB.peer.sin_family = AF_INET;
    dmrB.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dmrB.peer.sin_port = addrB.sin_port;

    memset(&last_txA, 0, sizeof(last_txA));
    memset(&last_txB, 0, sizeof(last_txB));
    memset(voice, 0x55, sizeof(voice));
    seqA = seqB = 0;
    memset(emb_rawA, 0, sizeof(emb_rawA));
    memset(emb_rawB, 0, sizeof(emb_rawB));

    argsA = (dmr_tx_args_t){ .peer = &dmrA, .bridge_dmrid = 3000001, .talker_rf_id = 7300391,
                             .tx_tg = 100, .seq = &seqA, .stream_id = 111, .last_tx = &last_txA,
                             .emb_raw = emb_rawA };
    argsB = (dmr_tx_args_t){ .peer = &dmrB, .bridge_dmrid = 3000002, .talker_rf_id = 7300391,
                             .tx_tg = 200, .seq = &seqB, .stream_id = 222, .last_tx = &last_txB,
                             .emb_raw = emb_rawB };

    /* Interleaved: A's n=0, B's n=0, then A's n=1, then B's n=1. */
    dmr_tx_send(&argsA, (uint8_t)(DMRD_FT_VOICE_SYNC << 4), voice);
    assert(recv_nb(fake_fdA, bufA, sizeof(bufA), 200) == 55);
    dmr_tx_send(&argsB, (uint8_t)(DMRD_FT_VOICE_SYNC << 4), voice);
    assert(recv_nb(fake_fdB, bufB, sizeof(bufB), 200) == 55);

    dmr_tx_send(&argsA, 1, voice);
    assert(recv_nb(fake_fdA, bufA, sizeof(bufA), 200) == 55);
    dmr_tx_send(&argsB, 1, voice);
    assert(recv_nb(fake_fdB, bufB, sizeof(bufB), 200) == 55);

    /* Isolated: A alone, nothing else touches emb_rawA in between. */
    memset(emb_rawA, 0, sizeof(emb_rawA));
    seqA = 0;
    dmr_tx_send(&argsA, (uint8_t)(DMRD_FT_VOICE_SYNC << 4), voice);
    assert(recv_nb(fake_fdA, buf_isolated, sizeof(buf_isolated), 200) == 55);
    dmr_tx_send(&argsA, 1, voice);
    assert(recv_nb(fake_fdA, buf_isolated, sizeof(buf_isolated), 200) == 55);

    /* Embedded LC fragment: wire offset 34-38. */
    assert(memcmp(bufA + 34, buf_isolated + 34, 5) == 0);

    close(dmrA.sock);
    close(dmrB.sock);
    close(fake_fdA);
    close(fake_fdB);
}

/* connect-PTT sequence (media/core_ysf_dmr.c): on DMR connect, wait
 * CONNECT_PTT_START_DELAY_MS, PTT to TG 4000 as a PRIVATE call for
 * CONNECT_PTT_MS, wait CONNECT_PTT_GAP_MS, then PTT to the real TG as a
 * normal GROUP call for CONNECT_PTT_MS. Drives the state machine via real
 * loopback UDP so the actual wire bytes (dst id, private bit) are checked,
 * not just internal state -- timestamps are rewound instead of sleeping so
 * the test runs instantly despite the multi-second real delays involved. */
static void test_connect_ptt_sequence_timing_and_privacy(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_peer_slot_t *dmr_slot;
    int dmr_id, fake_master_fd, n;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    uint8_t buf[64];

    fake_master_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fake_master_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fake_master_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(getsockname(fake_master_fd, (struct sockaddr *)&addr, &alen) == 0);

    media_router_init(&r);
    dmr_id = media_router_add_peer(&r, MEDIA_PEER_DMR);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 1;
    bus.slots[0].router_id = dmr_id;
    bus.slots[0].kind = MEDIA_PEER_DMR;
    bus.slots[0].open = 1;
    bus.slots[0].clear_dynamic_tg = 1;

    dmr_slot = &bus.slots[0];
    dmr_slot->u.dmr.sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(dmr_slot->u.dmr.sock >= 0);
    dmr_slot->u.dmr.dmrid = 7141001;
    dmr_slot->u.dmr.tg = 7141;
    dmr_slot->u.dmr.peer.sin_family = AF_INET;
    dmr_slot->u.dmr.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dmr_slot->u.dmr.peer.sin_port = addr.sin_port;
    dmr_slot->u.dmr.status = PEER_DMR_CONNECTED;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);
    core.leg_ysf_dmr.phase = MEDIA_CALL_IDLE;

    /* Rising edge -> phase 3 (initial 2s delay). Nothing sent yet. */
    core_ysf_dmr_poll_connect_ptt(&core);
    assert(dmr_slot->cp_active && dmr_slot->cp_phase == 3);
    assert(recv_nb(fake_master_fd, buf, sizeof(buf), 50) < 0);

    /* Past the 2s pre-delay -> arms the 4000 PTT (phase 0, not sent yet). */
    rewind_ms(&dmr_slot->cp_start, 2100);
    core_ysf_dmr_poll_connect_ptt(&core);
    assert(dmr_slot->cp_phase == 0);

    /* This tick sends the VHEAD bursts for the private PTT to 4000. */
    core_ysf_dmr_poll_connect_ptt(&core);
    assert(dmr_slot->cp_phase == 1);
    n = recv_nb(fake_master_fd, buf, sizeof(buf), 200);
    assert(n == 55 && memcmp(buf, "DMRD", 4) == 0);
    assert(buf[15] & DMRD_CALL_PRIVATE);
    assert(((buf[8] << 16) | (buf[9] << 8) | buf[10]) == 4000);

    /* Past the 500ms 4000 PTT -> finishes (VTERM) -> 4s gap (phase 2). */
    rewind_ms(&dmr_slot->cp_start, 600);
    rewind_ms(&core.leg_ysf_dmr.last_dmr_tx, 100);
    core_ysf_dmr_poll_connect_ptt(&core);
    assert(dmr_slot->cp_phase == 2 && dmr_slot->cp_clearing == 0);
    drain_all(fake_master_fd);

    /* Past the 4s gap -> arms the real-TG PTT (phase 0, not sent yet). */
    rewind_ms(&dmr_slot->cp_start, 4100);
    rewind_ms(&core.leg_ysf_dmr.last_dmr_tx, 100);
    core_ysf_dmr_poll_connect_ptt(&core);
    assert(dmr_slot->cp_phase == 0);

    /* This tick sends the VHEAD bursts for the real-TG PTT -- group call. */
    core_ysf_dmr_poll_connect_ptt(&core);
    n = recv_nb(fake_master_fd, buf, sizeof(buf), 200);
    assert(n == 55 && memcmp(buf, "DMRD", 4) == 0);
    assert(!(buf[15] & DMRD_CALL_PRIVATE));
    assert(((buf[8] << 16) | (buf[9] << 8) | buf[10]) == 7141);

    close(fake_master_fd);
    close(dmr_slot->u.dmr.sock);
}

/* A same-kind relay call (YSF<->YSF or DMR<->DMR) has no PCM/silence signal
 * to poll -- if the source's CALL_END/EOT is lost (common on a lossy RF/
 * hotspot link), the router's active_ingress lock must not stay stuck
 * forever, or every other peer is blocked from talking indefinitely. */
static void test_ysf_relay_stale_call_releases_router(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    int ysf1_id, ysf2_id;

    media_router_init(&r);
    ysf1_id = media_router_add_peer(&r, MEDIA_PEER_YSF);
    ysf2_id = media_router_add_peer(&r, MEDIA_PEER_YSF);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 2;
    bus.slots[0].router_id = ysf1_id;
    bus.slots[0].kind = MEDIA_PEER_YSF;
    bus.slots[0].open = 1;
    bus.slots[0].u.ysf.sock = -1;
    memset(bus.slots[0].u.ysf.callsign, ' ', sizeof(bus.slots[0].u.ysf.callsign));
    bus.slots[1].router_id = ysf2_id;
    bus.slots[1].kind = MEDIA_PEER_YSF;
    bus.slots[1].open = 1;
    bus.slots[1].u.ysf.sock = -1;
    memset(bus.slots[1].u.ysf.callsign, ' ', sizeof(bus.slots[1].u.ysf.callsign));

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    memcpy(frame.meta.netcall.net_src, "HP3ICC    ", 10);
    media_core_ingress(&core, ysf1_id, &frame);
    assert(media_router_active_ingress(&r) == ysf1_id);

    /* No further traffic (EOT lost) -- a tick before the hang window elapses
     * must leave the call active. */
    media_core_tick(&core);
    assert(media_router_active_ingress(&r) == ysf1_id);

    /* Past the hang window -- the watchdog must force-end and release. */
    rewind_ms(&core.last_ysf_relay_rx, 1600);
    media_core_tick(&core);
    assert(media_router_active_ingress(&r) == -1);

    /* Released means a new source can now take the lock. */
    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    memcpy(frame.meta.netcall.net_src, "OTHERSTA  ", 10);
    media_core_ingress(&core, ysf2_id, &frame);
    assert(media_router_active_ingress(&r) == ysf2_id);
}

/* YSF<->YSF relay must be transparent: the source radio's real DCH content
 * (radio model/serial/GPS live past the FICH span) must reach the far peer
 * byte-for-byte, unlike DMR->YSF/EchoLink->YSF synthesis which legitimately
 * fabricates that region (no real YSF frame to preserve there). Captures the
 * actual wire bytes over a real loopback socket, mirroring the connect-PTT
 * test's approach, rather than trusting internal state. */
static void test_ysf_relay_voice_frame_is_transparent(void)
{
    media_core_t core;
    media_router_t r;
    media_peer_bus_t bus;
    media_bus_frame_t frame;
    media_peer_slot_t *ysf2_slot;
    int ysf1_id, ysf2_id, fake_reflector_fd, n, i;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    uint8_t buf[160];
    uint8_t marker[40];

    fake_reflector_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fake_reflector_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fake_reflector_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(getsockname(fake_reflector_fd, (struct sockaddr *)&addr, &alen) == 0);

    media_router_init(&r);
    ysf1_id = media_router_add_peer(&r, MEDIA_PEER_YSF);
    ysf2_id = media_router_add_peer(&r, MEDIA_PEER_YSF);

    memset(&bus, 0, sizeof(bus));
    bus.router = &r;
    bus.n_slots = 2;
    bus.slots[0].router_id = ysf1_id;
    bus.slots[0].kind = MEDIA_PEER_YSF;
    bus.slots[0].open = 1;
    bus.slots[0].u.ysf.sock = -1;
    memset(bus.slots[0].u.ysf.callsign, ' ', sizeof(bus.slots[0].u.ysf.callsign));

    ysf2_slot = &bus.slots[1];
    ysf2_slot->router_id = ysf2_id;
    ysf2_slot->kind = MEDIA_PEER_YSF;
    ysf2_slot->open = 1;
    ysf2_slot->u.ysf.sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(ysf2_slot->u.ysf.sock >= 0);
    ysf2_slot->u.ysf.dgid = 82;
    memset(ysf2_slot->u.ysf.callsign, ' ', sizeof(ysf2_slot->u.ysf.callsign));
    memcpy(ysf2_slot->u.ysf.callsign, "N0CALL-LNK", 10);
    ysf2_slot->u.ysf.peer.sin_family = AF_INET;
    ysf2_slot->u.ysf.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ysf2_slot->u.ysf.peer.sin_port = addr.sin_port;

    media_core_init(&core);
    media_core_bind(&core, &r, &bus, NULL, NULL);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_CALL_BEGIN;
    frame.codec = CODEC_YSF_AMBE;
    memcpy(frame.meta.netcall.net_src, "HP3ICC    ", 10);
    media_core_ingress(&core, ysf1_id, &frame);
    n = recv_nb(fake_reflector_fd, buf, sizeof(buf), 200);
    assert(n == 155 && memcmp(buf, "YSFD", 4) == 0); /* HEADER, drained */

    for (i = 0; i < 40; i++)
        marker[i] = (uint8_t)(0x10 + i);

    memset(&frame, 0, sizeof(frame));
    frame.kind = MEDIA_FRAME_VOICE;
    frame.codec = CODEC_YSF_AMBE;
    memcpy(frame.meta.netcall.net_src, "HP3ICC    ", 10);
    memcpy(frame.payload.ysf_payload120 + 80, marker, sizeof(marker));
    media_core_ingress(&core, ysf1_id, &frame);

    n = recv_nb(fake_reflector_fd, buf, sizeof(buf), 200);
    assert(n == 155 && memcmp(buf, "YSFD", 4) == 0);
    /* payload120 lands at wire offset 35 (YSF_FICH_OFFSET_NET); the marker
     * at payload120[80:120) is wire[115:155) -- must survive byte-for-byte,
     * not be overwritten by DCH/FICH synthesis meant for non-YSF sources. */
    assert(memcmp(buf + 115, marker, sizeof(marker)) == 0);
    /* The one thing that IS always rewritten: DGID always follows this
     * destination peer's own configured value, never the source's. */
    {
        uint8_t fi, fn, ft, cm, dt;

        assert(ysf_fich_decode_fields(buf, &fi, &fn, &ft, &cm, &dt) == 0);
        assert(ysf_fich_get_dgid() == 82);
    }

    close(ysf2_slot->u.ysf.sock);
    close(fake_reflector_fd);
}

int main(void)
{
    test_init_defaults();
    test_bind_sets_use_vocoder();
    test_ingress_drops_when_blocked();
    test_dmr_call_begin_takes_ingress();
    test_shared_phase_no_cross_peer_preempt();
    test_el_dmr_call_begin_takes_rx_phase();
    test_el_dmr_process_el_audio_pauses_during_connect_ptt();
    test_el_ysf_call_begin_takes_rx_phase();
    test_multi_dmr_fanout_gets_distinct_tx_state();
    test_dmr_relay_two_peers();
    test_el_tx_fanout_dmr_and_ysf_concurrent();
    test_three_kind_bus_relay_direct_pcm_concurrent();
    test_connect_ptt_sequence_timing_and_privacy();
    test_ysf_relay_stale_call_releases_router();
    test_ysf_relay_voice_frame_is_transparent();
    test_dmr_tx_embedded_lc_independent_per_destination();
    printf("test_media_core: ok\n");
    return 0;
}

/*
 * Bridge engine — main loop with media router and peer bus.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine.h"

#include "adapters/dmr.h"
#include "adapters/ysf.h"
#include "config.h"
#include "log.h"
#include "media/codec_plan.h"
#include "media/core.h"
#include "media/peer_bus.h"
#include "media/router.h"
#include "peer_dmr.h"
#include "peer_echolink.h"
#include "peer_ysf.h"
#include "talker_alias.h"
#include "vocoder.h"

#include <string.h>
#include <time.h>

typedef struct {
    media_router_t      router;
    media_peer_bus_t    bus;
    media_codec_plan_t  plan;
    media_core_t       *core;
    int                 vocoder_open;
} engine_ctx_t;

static engine_ctx_t *g_alarm_ctx;

static media_peer_kind_t engine_peer_kind(adn_bridge_peer_type_t type)
{
    switch (type) {
    case ADN_BRIDGE_PEER_TYPE_DMR:
        return MEDIA_PEER_DMR;
    case ADN_BRIDGE_PEER_TYPE_YSF:
        return MEDIA_PEER_YSF;
    case ADN_BRIDGE_PEER_TYPE_ECHOLINK:
        return MEDIA_PEER_ECHOLINK;
    default:
        return MEDIA_PEER_DMR;
    }
}

static int engine_router_load_from_config(media_router_t *r, const adn_bridge_config_t *cfg)
{
    int i, id;
    media_peer_kind_t kind;

    media_router_init(r);
    for (i = 0; i < cfg->peer_count; i++) {
        const adn_bridge_peer_t *p = &cfg->peers[i];

        if (!p->type_set)
            continue;
        kind = engine_peer_kind(p->type);
        id = media_router_add_peer_cfg(r, kind, i, p->enabled);
        if (id < 0)
            return -1;
    }
    return 0;
}

static int engine_load_router_bus(engine_ctx_t *ctx, adn_bridge_config_t *cfg)
{
    if (engine_router_load_from_config(&ctx->router, cfg) != 0)
        return -1;
    media_peer_bus_init(&ctx->bus, &ctx->router);
    media_codec_plan_build(&ctx->router, &ctx->plan);
    return 0;
}

static void engine_poll_aliases(adn_bridge_config_t *cfg, engine_host_t *host,
                                time_t *last_poll, adn_bridge_aliases_t **core_aliases)
{
    time_t now = time(NULL);

    if ((cfg->aliases.stale_minutes <= 0 && cfg->aliases.reload_minutes <= 0)
        || now - *last_poll < 60)
        return;
    *last_poll = now;
    if (adn_bridge_aliases_maybe_refresh(&cfg->aliases, host->aliases) > 0 && core_aliases)
        *core_aliases = *host->aliases;
}

static int engine_start(engine_host_t *host, adn_bridge_config_t *cfg, engine_ctx_t *ctx)
{
    const adn_bridge_peer_t *dmr_p, *el_p;
    media_core_t *core = ctx->core;

    if (engine_load_router_bus(ctx, cfg) != 0)
        return -1;

    dmr_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_DMR);
    el_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_ECHOLINK);

    media_core_init(core);
    media_core_bind(core, &ctx->router, &ctx->bus, &ctx->plan, *host->aliases);
    media_core_set_bridge_dmrid(core, dmr_p ? dmr_p->u.dmr.dmrid : 0);
    if (el_p)
        media_core_set_el_gain(core, el_p->u.el.gain);

    if (ctx->plan.needs_vocoder) {
        if (!el_p || !el_p->u.el.vocoder_host[0] || el_p->u.el.vocoder_port <= 0)
            return -1;
        if (vocoder_open(&core->voc, el_p->u.el.vocoder_host, el_p->u.el.vocoder_port) < 0)
            return -1;
        ctx->vocoder_open = 1;
    }

    if (media_peer_bus_open_all(&ctx->bus, cfg) != 0) {
        if (ctx->vocoder_open)
            vocoder_close(&core->voc);
        return -1;
    }

    LOG_INFO("engine: %s (%d peers, ModeConv=%s, vocoder=%s)\n",
             adn_bridge_layout_name(cfg),
             media_router_peer_count(&ctx->router),
             ctx->plan.needs_modeconv ? "yes" : "no",
             ctx->plan.needs_vocoder ? "yes" : "no");
    return 0;
}

static void engine_stop(engine_ctx_t *ctx)
{
    media_peer_bus_sigint_all(&ctx->bus);
    media_peer_bus_close_all(&ctx->bus);
    if (ctx->vocoder_open)
        vocoder_close(&ctx->core->voc);
    ctx->core->router = NULL;
    ctx->core->bus = NULL;
}

static void engine_poll_dmr_slot(engine_ctx_t *ctx, media_peer_slot_t *slot)
{
    int from_dmr = 0, len;
    peer_dmr_t *dmr = &slot->u.dmr;

    peer_dmr_tick(dmr);
    len = peer_dmr_poll(dmr, 5, &from_dmr);
    if (!from_dmr || len <= 0)
        return;
    adapter_dmr_on_wire(ctx->core, slot->router_id, dmr, dmr->buf, len);
}

static void engine_poll_ysf_slot(engine_ctx_t *ctx, media_peer_slot_t *slot)
{
    int from_ysf = 0, len;
    peer_ysf_t *ysf = &slot->u.ysf;

    peer_ysf_tick(ysf);
    len = peer_ysf_poll(ysf, 5, &from_ysf);
    if (!from_ysf || len != 155)
        return;
    adapter_ysf_on_wire(ctx->core, slot->router_id, ysf, ysf->buf, len);
}

static void engine_poll_el_slot(media_peer_slot_t *slot)
{
    peer_echolink_t *el = &slot->u.el;

    peer_el_tick(el);
    peer_el_poll(el, 5);
}

static void engine_poll_bus(engine_ctx_t *ctx)
{
    int i;

    for (i = 0; i < ctx->bus.n_slots; i++) {
        media_peer_slot_t *slot = &ctx->bus.slots[i];

        if (!slot->open)
            continue;
        switch (slot->kind) {
        case MEDIA_PEER_DMR:
            engine_poll_dmr_slot(ctx, slot);
            break;
        case MEDIA_PEER_YSF:
            engine_poll_ysf_slot(ctx, slot);
            break;
        case MEDIA_PEER_ECHOLINK:
            engine_poll_el_slot(slot);
            break;
        default:
            break;
        }
    }
}

static void engine_step(engine_host_t *host, adn_bridge_config_t *cfg,
                        engine_ctx_t *ctx, time_t *last_alias_poll)
{
    engine_poll_bus(ctx);
    engine_poll_aliases(cfg, host, last_alias_poll, &ctx->core->aliases);
    media_core_poll_el_pcm(ctx->core);
    media_core_tick(ctx->core);
}

void engine_service_peer_alarms(void)
{
    int i;

    if (!g_alarm_ctx)
        return;
    for (i = 0; i < g_alarm_ctx->bus.n_slots; i++) {
        media_peer_slot_t *slot = &g_alarm_ctx->bus.slots[i];

        if (!slot->open)
            continue;
        switch (slot->kind) {
        case MEDIA_PEER_DMR:
            if (slot->u.dmr.sock >= 0)
                peer_dmr_on_alarm(&slot->u.dmr);
            break;
        case MEDIA_PEER_YSF:
            if (slot->u.ysf.sock >= 0)
                peer_ysf_on_alarm(&slot->u.ysf);
            break;
        default:
            break;
        }
    }
}

int engine_run(engine_host_t *host, adn_bridge_config_t *cfg, media_core_t *core)
{
    engine_ctx_t ctx;
    time_t last_alias_poll = time(NULL);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    if (engine_start(host, cfg, &ctx) != 0)
        return 1;

    g_alarm_ctx = &ctx;

    while (*host->keep_running) {
        if (host->service_alarm)
            host->service_alarm();
        engine_step(host, cfg, &ctx, &last_alias_poll);
    }

    g_alarm_ctx = NULL;
    engine_stop(&ctx);
    return 0;
}

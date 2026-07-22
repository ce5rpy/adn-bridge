/*
 * Bridge engine — main loop with media router.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine.h"

#include "bridge.h"
#include "log.h"
#include "media/router.h"
#include "peer_dmr.h"
#include "peer_echolink.h"
#include "peer_ysf.h"
#include "talker_alias.h"
#include "vocoder.h"

#include <string.h>
#include <time.h>

typedef struct {
    adn_bridge_layout_t layout;
    media_router_t router;
    adn_bridge_t *b;
    bridge_el_t *bel;
    int el_use_dmr;
    int el_use_ysf;
} engine_ctx_t;

static void engine_poll_aliases(adn_bridge_config_t *cfg, engine_host_t *host,
                                time_t *last_poll, adn_bridge_aliases_t **bel_aliases)
{
    time_t now = time(NULL);

    if ((cfg->aliases.stale_minutes <= 0 && cfg->aliases.reload_minutes <= 0)
        || now - *last_poll < 60)
        return;
    *last_poll = now;
    if (adn_bridge_aliases_maybe_refresh(&cfg->aliases, host->aliases) > 0 && bel_aliases)
        *bel_aliases = *host->aliases;
}

static void engine_register_el_peers(engine_ctx_t *ctx, int link_kind)
{
    int el_id, dmr_id, ysf_id;

    media_router_init(&ctx->router);
    el_id = media_router_add_peer(&ctx->router, MEDIA_PEER_ECHOLINK);
    dmr_id = -1;
    ysf_id = -1;
    if (link_kind == BRIDGE_EL_LINK_DMR)
        dmr_id = media_router_add_peer(&ctx->router, MEDIA_PEER_DMR);
    else if (link_kind == BRIDGE_EL_LINK_YSF)
        ysf_id = media_router_add_peer(&ctx->router, MEDIA_PEER_YSF);
    bridge_el_bind_router(ctx->bel, &ctx->router, el_id, dmr_id, ysf_id);
}

static void engine_register_ysf_dmr_peers(engine_ctx_t *ctx)
{
    int dmr_id, ysf_id;

    media_router_init(&ctx->router);
    dmr_id = media_router_add_peer(&ctx->router, MEDIA_PEER_DMR);
    ysf_id = media_router_add_peer(&ctx->router, MEDIA_PEER_YSF);
    bridge_bind_router(ctx->b, &ctx->router, dmr_id, ysf_id);
}

static int engine_start_ysf_dmr(engine_host_t *host, adn_bridge_config_t *cfg,
                                  engine_ctx_t *ctx)
{
    const adn_bridge_peer_t *dmr_p;
    const adn_bridge_peer_t *ysf_p;
    const adn_bridge_peer_dmr_t *dmr;
    const adn_bridge_peer_ysf_t *ysf;
    adn_bridge_t *b = ctx->b;

    dmr_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_DMR);
    ysf_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_YSF);
    if (!dmr_p || !ysf_p)
        return -1;

    dmr = &dmr_p->u.dmr;
    ysf = &ysf_p->u.ysf;

    bridge_init(b, dmr->options, *host->aliases, dmr->dmrid, dmr->clear_dynamic_tg);
    engine_register_ysf_dmr_peers(ctx);

    if (peer_ysf_open(&b->ysf, ysf->host, ysf->port, ysf->callsign, (uint8_t)ysf->dgid) < 0)
        return -1;
    if (peer_dmr_open(&b->dmr, dmr->host, dmr->port, dmr->callsign, dmr->dmrid, dmr->tg,
                      dmr->options, dmr->password, dmr->description, dmr->location) < 0) {
        peer_ysf_close(&b->ysf);
        return -1;
    }

    LOG_INFO("engine: YSF<->DMR (%d peers, ModeConv)\n",
             media_router_peer_count(&ctx->router));
    return 0;
}

static int engine_start_echolink(engine_host_t *host, adn_bridge_config_t *cfg,
                                   engine_ctx_t *ctx)
{
    const adn_bridge_peer_t *el_p;
    const adn_bridge_peer_t *dmr_p;
    const adn_bridge_peer_t *ysf_p;
    const adn_bridge_peer_el_t *el;
    bridge_el_t *bel = ctx->bel;
    int link_kind;

    el_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_ECHOLINK);
    dmr_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_DMR);
    ysf_p = adn_bridge_config_find_peer(cfg, ADN_BRIDGE_PEER_TYPE_YSF);
    if (!el_p)
        return -1;

    el = &el_p->u.el;
    ctx->el_use_dmr = (dmr_p != NULL);
    ctx->el_use_ysf = (ysf_p != NULL);
    link_kind = ctx->el_use_dmr ? BRIDGE_EL_LINK_DMR : BRIDGE_EL_LINK_YSF;

    bridge_el_init(bel, link_kind,
                   dmr_p ? dmr_p->u.dmr.options : "",
                   *host->aliases,
                   dmr_p ? dmr_p->u.dmr.dmrid : 0,
                   el->gain,
                   dmr_p ? dmr_p->u.dmr.clear_dynamic_tg : 0);
    engine_register_el_peers(ctx, link_kind);

    if (!el->vocoder_host[0] || el->vocoder_port <= 0)
        return -1;
    if (vocoder_open(&bel->voc, el->vocoder_host, el->vocoder_port) < 0)
        return -1;
    if (peer_el_open(&bel->el, el) < 0) {
        vocoder_close(&bel->voc);
        return -1;
    }

    if (ctx->el_use_dmr) {
        const adn_bridge_peer_dmr_t *dmr = &dmr_p->u.dmr;

        if (peer_dmr_open(&bel->dmr, dmr->host, dmr->port, dmr->callsign, dmr->dmrid,
                          dmr->tg, dmr->options, dmr->password, dmr->description,
                          dmr->location) < 0) {
            peer_el_close(&bel->el);
            vocoder_close(&bel->voc);
            return -1;
        }
        LOG_INFO("engine: EchoLink<->DMR (%d peers, vocoder %s:%d)\n",
                 media_router_peer_count(&ctx->router), el->vocoder_host, el->vocoder_port);
    }
    if (ctx->el_use_ysf) {
        const adn_bridge_peer_ysf_t *ysf = &ysf_p->u.ysf;

        if (peer_ysf_open(&bel->ysf, ysf->host, ysf->port, ysf->callsign,
                          (uint8_t)ysf->dgid) < 0) {
            if (ctx->el_use_dmr)
                peer_dmr_close(&bel->dmr);
            peer_el_close(&bel->el);
            vocoder_close(&bel->voc);
            return -1;
        }
        LOG_INFO("engine: EchoLink<->YSF (%d peers, vocoder %s:%d)\n",
                 media_router_peer_count(&ctx->router), el->vocoder_host, el->vocoder_port);
    }
    return 0;
}

static int engine_start(engine_host_t *host, adn_bridge_config_t *cfg,
                        engine_ctx_t *ctx)
{
    if (ctx->layout == ADN_BRIDGE_LAYOUT_YSF_DMR)
        return engine_start_ysf_dmr(host, cfg, ctx);
    if (ctx->layout == ADN_BRIDGE_LAYOUT_EL_DMR
        || ctx->layout == ADN_BRIDGE_LAYOUT_EL_YSF)
        return engine_start_echolink(host, cfg, ctx);
    LOG_ERROR("engine: unsupported layout\n");
    return -1;
}

static void engine_stop(engine_ctx_t *ctx)
{
    if (ctx->layout == ADN_BRIDGE_LAYOUT_YSF_DMR) {
        adn_bridge_t *b = ctx->b;

        peer_dmr_on_sigint(&b->dmr);
        peer_ysf_on_sigint(&b->ysf);
        bridge_bind_router(b, NULL, -1, -1);
        return;
    }

    if (ctx->layout == ADN_BRIDGE_LAYOUT_EL_DMR
        || ctx->layout == ADN_BRIDGE_LAYOUT_EL_YSF) {
        bridge_el_t *bel = ctx->bel;

        peer_el_on_sigint(&bel->el);
        if (ctx->el_use_dmr)
            peer_dmr_on_sigint(&bel->dmr);
        if (ctx->el_use_ysf)
            peer_ysf_on_sigint(&bel->ysf);
        vocoder_close(&bel->voc);
        bridge_el_bind_router(bel, NULL, -1, -1, -1);
    }
}

static void engine_step_ysf_dmr(engine_host_t *host, adn_bridge_config_t *cfg,
                                engine_ctx_t *ctx, time_t *last_alias_poll)
{
    adn_bridge_t *b = ctx->b;
    int from_dmr = 0, from_ysf = 0, len;

    peer_dmr_tick(&b->dmr);
    peer_ysf_tick(&b->ysf);
    engine_poll_aliases(cfg, host, last_alias_poll, &b->aliases);

    len = peer_dmr_poll(&b->dmr, 5, &from_dmr);
    if (from_dmr && len > 0) {
        if (len == 55 && memcmp(b->dmr.buf, "DMRD", 4) == 0)
            bridge_on_dmrd(b, b->dmr.buf, len);
        else if (len == DMRA_PACKET_LEN && memcmp(b->dmr.buf, "DMRA", 4) == 0)
            bridge_on_dmra(b, b->dmr.buf, len);
    }

    len = peer_ysf_poll(&b->ysf, 5, &from_ysf);
    if (from_ysf && len > 0 && len == 155)
        bridge_on_ysfd(b, b->ysf.buf, len);

    bridge_tick(b);
}

static void engine_step_echolink(engine_host_t *host, adn_bridge_config_t *cfg,
                                 engine_ctx_t *ctx, time_t *last_alias_poll)
{
    bridge_el_t *bel = ctx->bel;
    int from_dmr = 0, from_ysf = 0, len;

    peer_el_tick(&bel->el);
    if (ctx->el_use_dmr)
        peer_dmr_tick(&bel->dmr);
    if (ctx->el_use_ysf)
        peer_ysf_tick(&bel->ysf);

    engine_poll_aliases(cfg, host, last_alias_poll, &bel->aliases);

    peer_el_poll(&bel->el, 5);
    if (ctx->el_use_dmr)
        bridge_el_process_el_audio(bel);
    else
        bridge_el_process_el_to_ysf(bel);

    if (ctx->el_use_dmr) {
        len = peer_dmr_poll(&bel->dmr, 5, &from_dmr);
        if (from_dmr && len == 55 && memcmp(bel->dmr.buf, "DMRD", 4) == 0)
            bridge_el_on_dmrd(bel, bel->dmr.buf, len);
    }
    if (ctx->el_use_ysf) {
        len = peer_ysf_poll(&bel->ysf, 5, &from_ysf);
        if (from_ysf && len == 155)
            bridge_el_on_ysfd(bel, bel->ysf.buf, len);
    }
    bridge_el_tick(bel);
}

static void engine_step(engine_host_t *host, adn_bridge_config_t *cfg,
                        engine_ctx_t *ctx, time_t *last_alias_poll)
{
    if (ctx->layout == ADN_BRIDGE_LAYOUT_YSF_DMR)
        engine_step_ysf_dmr(host, cfg, ctx, last_alias_poll);
    else
        engine_step_echolink(host, cfg, ctx, last_alias_poll);
}

int engine_run(engine_host_t *host, adn_bridge_config_t *cfg,
               adn_bridge_t *b, bridge_el_t *bel)
{
    engine_ctx_t ctx;
    time_t last_alias_poll = time(NULL);

    memset(&ctx, 0, sizeof(ctx));
    ctx.layout = adn_bridge_config_layout(cfg);
    ctx.b = b;
    ctx.bel = bel;

    if (engine_start(host, cfg, &ctx) != 0)
        return 1;

    while (*host->keep_running) {
        host->service_alarm();
        engine_step(host, cfg, &ctx, &last_alias_poll);
    }

    engine_stop(&ctx);
    return 0;
}

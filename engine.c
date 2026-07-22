/*
 * Bridge engine — EchoLink modes with two-peer media router.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine.h"

#include "adapters/el.h"
#include "log.h"
#include "media/router.h"
#include "peer_dmr.h"
#include "peer_echolink.h"
#include "peer_ysf.h"
#include "vocoder.h"

#include <string.h>
#include <time.h>

static void engine_register_peers(media_router_t *r, adn_bridge_config_t *cfg,
                                bridge_el_t *bel, int *el_id, int *dmr_id, int *ysf_id)
{
    media_router_init(r);
    *el_id = media_router_add_peer(r, MEDIA_PEER_ECHOLINK);
    *dmr_id = -1;
    *ysf_id = -1;
    if (cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR)
        *dmr_id = media_router_add_peer(r, MEDIA_PEER_DMR);
    else if (cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF)
        *ysf_id = media_router_add_peer(r, MEDIA_PEER_YSF);
    bridge_el_bind_router(bel, r, *el_id, *dmr_id, *ysf_id);
}

int engine_run_echolink(engine_host_t *host, adn_bridge_config_t *cfg,
                        bridge_el_t *bel)
{
    media_router_t router;
    int el_id, dmr_id, ysf_id;
    int use_dmr = (cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_DMR);
    int use_ysf = (cfg->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF);
    time_t last_alias_poll = time(NULL);

    bridge_el_init(bel, cfg->mode, cfg->dmr_options, *host->aliases, cfg->dmrid,
                   cfg->echolink.gain, cfg->dmr_clear_dynamic_tg);
    engine_register_peers(&router, cfg, bel, &el_id, &dmr_id, &ysf_id);

    if (vocoder_open(&bel->voc, cfg->vocoder.host, cfg->vocoder.port) < 0)
        return 1;
    if (peer_el_open(&bel->el, &cfg->echolink) < 0)
        return 1;

    if (use_dmr) {
        if (peer_dmr_open(&bel->dmr, cfg->dmr_host, cfg->dmr_port, cfg->callsign,
                          cfg->dmrid, cfg->dmr_tg, cfg->dmr_options,
                          cfg->dmr_password,
                          cfg->description, cfg->location) < 0)
            return 1;
        LOG_INFO("engine: EchoLink<->DMR (%d peers, vocoder %s:%d)\n",
                 media_router_peer_count(&router),
                 cfg->vocoder.host, cfg->vocoder.port);
    }
    if (use_ysf) {
        char ysf_cs[10];

        bridge_el_format_callsign10(ysf_cs, cfg->echolink.callsign);
        if (peer_ysf_open(&bel->ysf, cfg->ysf_host, cfg->ysf_port, ysf_cs,
                          (uint8_t)cfg->dgid) < 0)
            return 1;
        LOG_INFO("engine: EchoLink<->YSF (%d peers, vocoder %s:%d)\n",
                 media_router_peer_count(&router),
                 cfg->vocoder.host, cfg->vocoder.port);
    }

    while (*host->keep_running) {
        int from_dmr = 0, from_ysf = 0, len;
        time_t now;

        host->service_alarm();
        peer_el_tick(&bel->el);
        if (use_dmr)
            peer_dmr_tick(&bel->dmr);
        if (use_ysf)
            peer_ysf_tick(&bel->ysf);

        now = time(NULL);
        if ((cfg->aliases.stale_minutes > 0 || cfg->aliases.reload_minutes > 0)
            && now - last_alias_poll >= 60) {
            last_alias_poll = now;
            if (adn_bridge_aliases_maybe_refresh(&cfg->aliases, host->aliases) > 0)
                bel->aliases = *host->aliases;
        }

        peer_el_poll(&bel->el, 5);
        if (use_dmr)
            bridge_el_process_el_audio(bel);
        else
            bridge_el_process_el_to_ysf(bel);

        if (use_dmr) {
            len = peer_dmr_poll(&bel->dmr, 5, &from_dmr);
            if (from_dmr && len == 55 && memcmp(bel->dmr.buf, "DMRD", 4) == 0)
                bridge_el_on_dmrd(bel, bel->dmr.buf, len);
        }
        if (use_ysf) {
            len = peer_ysf_poll(&bel->ysf, 5, &from_ysf);
            if (from_ysf && len == 155)
                bridge_el_on_ysfd(bel, bel->ysf.buf, len);
        }
        bridge_el_tick(bel);
    }

    peer_el_on_sigint(&bel->el);
    if (use_dmr)
        peer_dmr_on_sigint(&bel->dmr);
    if (use_ysf)
        peer_ysf_on_sigint(&bel->ysf);
    vocoder_close(&bel->voc);
    bridge_el_bind_router(bel, NULL, -1, -1, -1);
    return 0;
}

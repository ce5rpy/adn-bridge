/*
 * EchoLink protocol adapter — talker identity and EL-side hooks.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adapters/el.h"

#include "config.h"
#include "log.h"
#include "media/identity.h"
#include "peer_echolink.h"

#include <stdio.h>
#include <string.h>

void bridge_el_format_callsign10(char out[10], const char *src)
{
    identity_format_full_callsign10(out, src);
}

void adapter_el_resolve_talker(bridge_el_t *b)
{
    const char *raw = peer_el_remote_talker(&b->el);
    char base[16];
    char talker10[10];
    int id = 0;

    if (!raw || !raw[0])
        raw = b->el.callsign;
    identity_callsign_base(raw, base);
    if (!base[0]) {
        identity_callsign_base(b->el.callsign, base);
        raw = b->el.callsign;
    }

    identity_format_base_callsign10(talker10, base);
    id = identity_callsign10_to_dmrid((const uint8_t *)talker10);
    if (id <= 0 && base[0] && b->aliases)
        id = identity_lookup_alias_id(b->aliases, base);

    if (b->mode == ADN_BRIDGE_MODE_ECHOLINK_YSF) {
        char prev[10];

        memcpy(prev, b->net_src, 10);
        bridge_el_format_callsign10(b->net_src, raw);
        b->el_rf_id = id > 0 ? id : b->bridge_dmrid;
        if (memcmp(prev, b->net_src, 10) != 0)
            LOG_YSF_INFO("EL->YSF talker raw=%s base=%s\n", raw, base[0] ? base : "?");
        return;
    }

    if (id > 0) {
        memcpy(b->net_src, talker10, 10);
        b->el_rf_id = id;
        LOG_DMR_INFO("EL->DMR talker %s -> id %d (alias)\n", base, id);
        return;
    }

    if (b->dmr.dmrid > 0) {
        memcpy(b->net_src, b->dmr.callsign, 10);
        b->el_rf_id = b->dmr.dmrid;
        LOG_DMR_INFO("EL->DMR talker %s unknown -> bridge %.10s id %d\n",
                     base[0] ? base : "?", b->net_src, b->el_rf_id);
        return;
    }
    if (b->bridge_dmrid > 0) {
        b->el_rf_id = b->bridge_dmrid;
        LOG_DMR_INFO("EL->DMR talker %s unknown -> bridge id %d\n",
                     base[0] ? base : "?", b->el_rf_id);
        return;
    }
    b->el_rf_id = 0;
    LOG_DMR_WARNING("EL->DMR talker %s: no DMR id (alias miss, no bridge dmrid)\n",
                    base[0] ? base : "?");
}

void adapter_el_set_ysf_talker_name(bridge_el_t *b)
{
    char talker[16];
    char name[32];

    identity_wire_call_to_cstr(talker, b->net_src);
    if (!talker[0]) {
        peer_el_set_talker_name(&b->el, NULL);
        return;
    }
    snprintf(name, sizeof(name), "%s (%s)", b->el.callsign, talker);
    peer_el_set_talker_name(&b->el, name);
}

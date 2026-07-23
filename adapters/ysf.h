/*
 * YSF protocol adapter — YSF↔DMR and EchoLink↔YSF bridges.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_YSF_H
#define ADAPTER_YSF_H

#include <stdint.h>

#include "bridge.h"
#include "media/core.h"
#include "peer_ysf.h"

void adapter_ysf_dmr_reset_call(adn_bridge_t *b);

void adapter_ysf_on_ysfd_ysf(adn_bridge_t *b, const uint8_t *pkt, int len);
int adapter_ysf_emit_from_conv_ysf(adn_bridge_t *b);

/* Wire parse only (FICH decode + DGID filter, a wire rule, not
 * a session decision), hands off to media_core_ingress. */
void adapter_ysf_on_wire(media_core_t *core, int src_router_id, peer_ysf_t *ysf,
                         const uint8_t *pkt, int len);

#endif

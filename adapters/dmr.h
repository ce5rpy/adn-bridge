/*
 * DMR protocol adapter (RX/TX FSM) — YSF↔DMR and EchoLink bridges.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_DMR_H
#define ADAPTER_DMR_H

#include <stdint.h>

#include "bridge.h"
#include "media/core.h"
#include "peer_dmr.h"

uint8_t adapter_dmr_slot_bit_from_options(const char *options);

/* YSF <-> DMR bridge path (legacy — driven by bridge.c until Fase 4 cutover) */
void adapter_dmr_on_dmrd_ysf(adn_bridge_t *b, const uint8_t *pkt, int len);
void adapter_dmr_on_dmra_ysf(adn_bridge_t *b, const uint8_t *pkt, int len);
void adapter_dmr_poll_connect_ptt_ysf(adn_bridge_t *b);
void adapter_dmr_abort_connect_ptt_ysf(adn_bridge_t *b);
void adapter_dmr_emit_from_conv_ysf(adn_bridge_t *b);

/* Wire parse only, hands off to media_core_ingress. No
 * ModeConv, vocoder, or router fan-out calls here. */
void adapter_dmr_on_wire(media_core_t *core, int src_router_id, peer_dmr_t *dmr,
                        const uint8_t *pkt, int len);

#endif

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

uint8_t adapter_dmr_slot_bit_from_options(const char *options);

/* YSF <-> DMR (bridge.c shim) */
void adapter_dmr_on_dmrd_ysf(adn_bridge_t *b, const uint8_t *pkt, int len);
void adapter_dmr_on_dmra_ysf(adn_bridge_t *b, const uint8_t *pkt, int len);
void adapter_dmr_poll_connect_ptt_ysf(adn_bridge_t *b);
void adapter_dmr_abort_connect_ptt_ysf(adn_bridge_t *b);
void adapter_dmr_emit_from_conv_ysf(adn_bridge_t *b);

#endif

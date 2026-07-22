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

void adapter_ysf_dmr_reset_call(adn_bridge_t *b);

void adapter_ysf_on_ysfd_ysf(adn_bridge_t *b, const uint8_t *pkt, int len);
int adapter_ysf_emit_from_conv_ysf(adn_bridge_t *b);

#endif

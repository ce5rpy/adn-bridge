/*
 * DMR wire adapter: parse only, hand off to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_DMR_H
#define ADAPTER_DMR_H

#include <stdint.h>

#include "media/core.h"
#include "peer_dmr.h"

/* No ModeConv, vocoder, or router fan-out calls here. */
void adapter_dmr_on_wire(media_core_t *core, int src_router_id, peer_dmr_t *dmr,
                        const uint8_t *pkt, int len);

#endif

/*
 * YSF wire adapter: parse only, hand off to media_core.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_YSF_H
#define ADAPTER_YSF_H

#include <stdint.h>

#include "media/core.h"
#include "peer_ysf.h"

/* FICH decode + DGID filter (a wire rule, not a session decision) live here.
 * No ModeConv, vocoder, or router fan-out calls. */
void adapter_ysf_on_wire(media_core_t *core, int src_router_id, peer_ysf_t *ysf,
                         const uint8_t *pkt, int len);

#endif

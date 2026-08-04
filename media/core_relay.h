/*
 * media_core — same-protocol relay (DMR<->DMR, YSF<->YSF, EchoLink<->EchoLink):
 * "the diagonal" from docs-priv/media-bus-migration.md's N-way bus design.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_CORE_RELAY_H
#define ADN_MEDIA_CORE_RELAY_H

#include "media/core.h"

void core_relay_dmr_to_dmr(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void core_relay_ysf_to_ysf(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
/* Polled (mirrors media_core_poll_el_pcm) — no wire frame to classify. */
void core_relay_el_to_el(media_core_t *core);
/* Force-ends a DMR<->DMR or YSF<->YSF relay call stuck without a CALL_END
 * (lost VTERM/EOT). Call once per media_core_tick. */
void core_relay_check_stale(media_core_t *core);

#endif

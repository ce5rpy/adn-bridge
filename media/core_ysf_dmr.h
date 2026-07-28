/*
 * media_core — YSF<->DMR pathway (ported from bridge.c + adapters/dmr.c + adapters/ysf.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_CORE_YSF_DMR_H
#define ADN_MEDIA_CORE_YSF_DMR_H

#include "media/core.h"

void core_ysf_dmr_merge_dmra(media_core_t *core, const media_bus_frame_t *frame);
void core_ysf_dmr_ingress_dmr(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void core_ysf_dmr_ingress_ysf(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void core_ysf_dmr_tick(media_core_t *core);
void core_ysf_dmr_poll_connect_ptt(media_core_t *core);

#endif

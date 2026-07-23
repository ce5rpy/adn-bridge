/*
 * media_core — EchoLink<->DMR and EchoLink<->YSF pathways (ported from bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_CORE_ECHOLINK_H
#define ADN_MEDIA_CORE_ECHOLINK_H

#include "media/core.h"

/* DMR -> EL (ingress from wire) and EL -> DMR (polled PCM). */
void core_el_dmr_ingress_dmr(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void core_el_dmr_process_el_audio(media_core_t *core);

/* YSF -> EL (ingress from wire) and EL -> YSF (polled PCM). */
void core_el_ysf_ingress_ysf(media_core_t *core, int src_router_id, const media_bus_frame_t *frame);
void core_el_ysf_process_el_audio(media_core_t *core);

/* Paces ModeConv/DMR TX, EL hangtime, DMR/YSF RX hangtime — derives which
 * pairing is active from the router's enabled peer kinds, not a stored
 * layout enum (mirrors bridge_el_tick's link_kind branches, generalized). */
void core_el_tick(media_core_t *core);

#endif

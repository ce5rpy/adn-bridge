/*
 * media_core — generic PCM-native peer (EchoLink, ALSA, ...) <-> DMR/YSF
 * bridge via vocoder + ModeConv. One engine serves any number of PCM-native
 * peer kinds/instances; each keeps its own session state on its own
 * media_peer_slot_t (media/pcm_leg.h) so 2+ active at once never corrupt
 * each other. Replaces the old media/core_echolink.c, which hardcoded this
 * same logic for "the" EchoLink peer only.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_CORE_PCM_BRIDGE_H
#define ADN_MEDIA_CORE_PCM_BRIDGE_H

#include "media/core.h"

/* DMR -> every enabled slot of pcm_kind (ingress from wire). */
void core_pcm_bridge_ingress_dmr(media_core_t *core, media_peer_kind_t pcm_kind,
                                  int src_router_id, const media_bus_frame_t *frame);

/* YSF -> every enabled slot of pcm_kind (ingress from wire). */
void core_pcm_bridge_ingress_ysf(media_core_t *core, media_peer_kind_t pcm_kind,
                                  int src_router_id, const media_bus_frame_t *frame);

/* Every enabled slot of pcm_kind's mic -> DMR and/or YSF (polled PCM, not
 * wire-classified) -- one capture+encode pass per slot, fanned to whichever
 * destination pathway(s) are eligible so both can run concurrently. */
void core_pcm_bridge_poll(media_core_t *core, media_peer_kind_t pcm_kind);

/* Paces ModeConv/DMR TX, connect-PTT, hang timers, for every enabled slot of
 * pcm_kind. */
void core_pcm_bridge_tick(media_core_t *core, media_peer_kind_t pcm_kind);

#endif

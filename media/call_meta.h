/*
 * Shared call metadata (wire callsign slots) for bridge modules.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_CALL_META_H
#define ADN_CALL_META_H

#include <stddef.h>

#define BRIDGE_CALLSIGN_LEN 10

typedef struct {
    char net_src[BRIDGE_CALLSIGN_LEN];
    char net_dst[BRIDGE_CALLSIGN_LEN];
} bridge_call_meta_t;

void bridge_call_meta_clear(bridge_call_meta_t *meta);

/* Session phase for a cross-kind pathway leg (media/core.h, media/pcm_leg.h).
 * Lives here (not media/core.h) so media/pcm_leg.h can use it without a
 * circular include back through media/peer_bus.h -> media/core.h. */
typedef enum {
    MEDIA_CALL_IDLE = 0,
    MEDIA_CALL_TX_TO_PEER,   /* ingress from one peer, fanning out to others */
    MEDIA_CALL_RX_FROM_PEER, /* single-peer layouts: peer -> bus, symmetric leg */
} media_call_phase_t;

#endif

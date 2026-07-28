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

#endif

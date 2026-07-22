/*
 * Shared call metadata (wire callsign slots) for bridge modules.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/call_meta.h"

#include <string.h>

void bridge_call_meta_clear(bridge_call_meta_t *meta)
{
    if (!meta)
        return;
    memset(meta->net_src, ' ', BRIDGE_CALLSIGN_LEN);
    memset(meta->net_dst, ' ', BRIDGE_CALLSIGN_LEN);
}

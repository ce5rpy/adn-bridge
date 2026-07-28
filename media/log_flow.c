/*
 * Dynamic peer-flow labels for logs (src->dst from router kinds).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/log_flow.h"

#include "adapters/peer_plugin.h"

#include <stdio.h>

const char *media_flow_label(media_peer_kind_t src, media_peer_kind_t dst)
{
    static char buf[MEDIA_FLOW_LABEL_MAX];

    snprintf(buf, sizeof(buf), "%s->%s",
             peer_plugin_label(src), peer_plugin_label(dst));
    return buf;
}

/*
 * Dynamic peer-flow labels for logs (src->dst from router kinds).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_MEDIA_LOG_FLOW_H
#define ADN_MEDIA_LOG_FLOW_H

#include "media/router.h"

#define MEDIA_FLOW_LABEL_MAX 32

/* Single-threaded engine: static buffer. "dmr->echolink", etc. */
const char *media_flow_label(media_peer_kind_t src, media_peer_kind_t dst);

#endif

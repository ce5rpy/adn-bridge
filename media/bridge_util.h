/*
 * Shared timing / stream helpers for bridge modules.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_BRIDGE_UTIL_H
#define ADN_BRIDGE_UTIL_H

#include <stdint.h>
#include <time.h>

void bridge_stamp_now(struct timespec *ts);
uint32_t bridge_new_stream_id(void);
int bridge_ms_elapsed(const struct timespec *since, int interval_ms);
long bridge_ms_since(const struct timespec *since);
int bridge_dbg_periodic(int *n);

#endif

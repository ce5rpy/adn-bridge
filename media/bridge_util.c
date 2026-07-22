/*
 * Shared timing / stream helpers for bridge modules.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/bridge_util.h"

#include <stdlib.h>
#include <time.h>

void bridge_stamp_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

uint32_t bridge_new_stream_id(void)
{
    static int seeded;

    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    return (uint32_t)rand() | 1U;
}

int bridge_ms_elapsed(const struct timespec *since, int interval_ms)
{
    struct timespec now;
    long elapsed;

    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed = (now.tv_sec - since->tv_sec) * 1000L
            + (now.tv_nsec - since->tv_nsec) / 1000000L;
    return elapsed >= interval_ms;
}

long bridge_ms_since(const struct timespec *since)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L
         + (now.tv_nsec - since->tv_nsec) / 1000000L;
}

int bridge_dbg_periodic(int *n)
{
    (*n)++;
    return (*n <= 2 || (*n % 20) == 0);
}

/*
 * Bridge engine — main loop with media router.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_ENGINE_H
#define ADN_ENGINE_H

#include <signal.h>

#include "aliases.h"
#include "config.h"
#include "media/core.h"

typedef struct {
    volatile sig_atomic_t *keep_running;
    void (*service_alarm)(void);
    adn_bridge_aliases_t **aliases;
} engine_host_t;

/* Start peers from [peer.*] layout, run until *keep_running clears, then shutdown. */
int engine_run(engine_host_t *host, adn_bridge_config_t *cfg, media_core_t *core);

/* Iterate open bus peers for SIGALRM keepalives (set during engine_run). */
void engine_service_peer_alarms(void);

#endif

/*
 * Bridge engine — main loop with media router (Phase 3+).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_ENGINE_H
#define ADN_ENGINE_H

#include <signal.h>

#include "bridge.h"
#include "bridge_el.h"
#include "config.h"
#include "aliases.h"

typedef struct {
    volatile sig_atomic_t *keep_running;
    void (*service_alarm)(void);
    adn_bridge_aliases_t **aliases;
} engine_host_t;

/* EchoLink ↔ DMR / EchoLink ↔ YSF. */
int engine_run_echolink(engine_host_t *host, adn_bridge_config_t *cfg,
                        bridge_el_t *bel);

/* YSF ↔ DMR (ModeConv). */
int engine_run_ysf_dmr(engine_host_t *host, adn_bridge_config_t *cfg,
                       adn_bridge_t *b);

/* Dispatch by cfg->mode. */
int engine_run(engine_host_t *host, adn_bridge_config_t *cfg,
               adn_bridge_t *b, bridge_el_t *bel);

#endif

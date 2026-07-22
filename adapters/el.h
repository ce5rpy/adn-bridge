/*
 * EchoLink protocol adapter — talker identity and EL-side hooks.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAPTER_EL_H
#define ADAPTER_EL_H

#include "bridge_el.h"

void adapter_el_resolve_talker(bridge_el_t *b);
void adapter_el_set_ysf_talker_name(bridge_el_t *b);

#endif

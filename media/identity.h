/*
 * Callsign / alias identity helpers (bridge.c + bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_IDENTITY_H
#define ADN_IDENTITY_H

#include <stdint.h>

#include "aliases.h"
#include "media/call_meta.h"

void identity_dbg_label10(char out[11], const uint8_t raw[10]);
void identity_format_id_callsign10(char out[10], int id);
void identity_format_tg_dst10(char out[10], int tg);
void identity_format_base_callsign10(char out[10], const char *src);
void identity_format_full_callsign10(char out[10], const char *src);
void identity_callsign_base(const char *src, char out[16]);
void identity_callsign_base10(const char src10[10], char out[16]);
void identity_callsign_base_src(const char *src, char out[16]);
void identity_wire_call_to_cstr(char out[16], const char src[10]);
int identity_callsign10_to_dmrid(const uint8_t cs[10]);

int identity_lookup_alias_id(adn_bridge_aliases_t *aliases, const char base_cs[16]);
int identity_lookup_dmr_callsign(adn_bridge_aliases_t *aliases, int rf, char out[10]);

typedef struct {
    adn_bridge_aliases_t *aliases;
    int bridge_dmrid;
    const char *bridge_callsign; /* 10-char padded [dmr] callsign */
    const char *dmra_text;
    int dmra_rf;
} identity_dmr_ctx_t;

void identity_resolve_dmr_to_ysf(bridge_call_meta_t *meta, const identity_dmr_ctx_t *ctx,
                                 int rf, int dst);

typedef struct {
    adn_bridge_aliases_t *aliases;
    int bridge_dmrid;
    const char *bridge_callsign; /* 10-char padded */
} identity_ysf_ctx_t;

int identity_assign_ysf_talker(bridge_call_meta_t *meta, int *ysf_rf_id,
                               const identity_ysf_ctx_t *ctx, const char *src);
int identity_resolve_ysf_header(bridge_call_meta_t *meta, int *ysf_rf_id,
                                const identity_ysf_ctx_t *ctx, const uint8_t *pkt155);

#endif

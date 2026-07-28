/*
 * Callsign / alias identity helpers (bridge.c + bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/identity.h"

#include "log.h"
#include "media/log_flow.h"
#include "mmdvm/ysfpayload_wrap.h"
#include "session/ysf_tx.h"
#include "ysf_fich.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void identity_dbg_label10(char out[11], const uint8_t raw[10])
{
    int i;

    for (i = 0; i < 10; i++)
        out[i] = (raw[i] >= 32 && raw[i] < 127) ? (char)raw[i] : '.';
    out[10] = '\0';
}

static void identity_format_id_callsign10(char out[10], int id)
{
    char tmp[12];
    int n;

    memset(out, ' ', 10);
    snprintf(tmp, sizeof(tmp), "%d", id);
    n = (int)strlen(tmp);
    if (n > 10)
        n = 10;
    memcpy(out, tmp, (size_t)n);
}

static void identity_format_tg_dst10(char out[10], int tg)
{
    char tmp[16];
    int n;

    memset(out, ' ', 10);
    snprintf(tmp, sizeof(tmp), "TG %d", tg);
    n = (int)strlen(tmp);
    if (n > 10)
        n = 10;
    memcpy(out, tmp, (size_t)n);
}

void identity_format_base_callsign10(char out[10], const char *src)
{
    int i, j = 0;

    memset(out, ' ', 10);
    if (!src)
        return;
    for (i = 0; src[i] && j < 10; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ' || c == '\t')
            continue;
        if (c == '-' || c == '/')
            break;
        out[j++] = (char)toupper(c);
    }
}

void identity_format_full_callsign10(char out[10], const char *src)
{
    int i, j = 0;

    memset(out, ' ', 10);
    if (!src)
        return;
    for (i = 0; src[i] && j < 10; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ' || c == '\t')
            continue;
        out[j++] = (char)toupper(c);
    }
}

void identity_callsign_base(const char *src, char out[16])
{
    int i, j = 0;

    out[0] = '\0';
    if (!src)
        return;
    for (i = 0; src[i] && j < 15; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ' || c == '\t') {
            if (j == 0)
                continue;
            break;
        }
        if (c == '-' || c == '/')
            break;
        out[j++] = (char)toupper(c);
    }
    out[j] = '\0';
}

void identity_callsign_base10(const char src10[10], char out[16])
{
    int i, j = 0;

    if (!out) {
        return;
    }
    out[0] = '\0';
    if (!src10) {
        return;
    }
    for (i = 0; i < 10; i++) {
        unsigned char c = (unsigned char)src10[i];

        if (c == ' ' || c == '\0') {
            break;
        }
        if (c == '-' || c == '/') {
            break;
        }
        out[j++] = (char)toupper(c);
        if (j >= 15) {
            break;
        }
    }
    if (j == 0) {
        strncpy(out, "N0CALL", 15);
        out[15] = '\0';
    } else {
        out[j] = '\0';
    }
}

static void identity_callsign_base_src(const char *src, char out[16])
{
    char pad[10];
    int i;

    memset(pad, ' ', sizeof(pad));
    if (src) {
        for (i = 0; i < 10 && src[i]; i++) {
            pad[i] = src[i];
        }
    }
    identity_callsign_base10(pad, out);
}

void identity_wire_call_to_cstr(char out[16], const char src[10])
{
    int i, n = 0;

    out[0] = '\0';
    if (!src)
        return;
    for (i = 0; i < 10 && n < 15; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '\0')
            break;
        if (c == ' ' || c == '\t') {
            if (n == 0)
                continue;
            break;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

static int label_is_numeric_dmrid(const uint8_t cs[10])
{
    int i, has_digit = 0;

    for (i = 0; i < 10; i++) {
        if (cs[i] == ' ')
            continue;
        if (cs[i] >= '0' && cs[i] <= '9')
            has_digit = 1;
        else
            return 0;
    }
    return has_digit;
}

int identity_callsign10_to_dmrid(const uint8_t cs[10])
{
    char digits[16];
    int i, j = 0;

    if (!label_is_numeric_dmrid(cs))
        return 0;

    for (i = 0; i < 10; i++) {
        if (cs[i] >= '0' && cs[i] <= '9')
            digits[j++] = (char)cs[i];
    }
    if (j == 0)
        return 0;
    digits[j] = '\0';
    return atoi(digits);
}

int identity_lookup_alias_id(adn_bridge_aliases_t *aliases, const char base_cs[16])
{
    if (!aliases || !base_cs || !base_cs[0])
        return 0;
    return adn_bridge_alias_lookup_id(aliases, base_cs);
}

int identity_lookup_dmr_callsign(adn_bridge_aliases_t *aliases, int rf, char out[10])
{
    int i;

    if (!aliases || rf <= 0)
        return 0;
    if (adn_bridge_alias_lookup_callsign(aliases, rf, out))
        return 1;
    if (rf > 9999999 && adn_bridge_alias_lookup_callsign(aliases, rf / 100, out))
        return 1;
    if (rf >= 10000 && rf <= 99999) {
        for (i = 0; i < 100; i++) {
            if (adn_bridge_alias_lookup_callsign(aliases, rf * 100 + i, out))
                return 1;
        }
    }
    return 0;
}

/* Best-effort display callsign for a DMR radio ID: subscriber DB lookup,
 * else the numeric ID itself (same fallback as identity_resolve_dmr_to_ysf),
 * trimmed to a clean C string. For UI/roster labels, not wire framing. */
void identity_dmr_display_callsign(adn_bridge_aliases_t *aliases, int rf, char out[16])
{
    char cs10[10];

    if (rf <= 0) {
        out[0] = '\0';
        return;
    }
    if (!identity_lookup_dmr_callsign(aliases, rf, cs10))
        identity_format_id_callsign10(cs10, rf);
    identity_wire_call_to_cstr(out, cs10);
}

void identity_resolve_dmr_to_ysf(bridge_call_meta_t *meta, const identity_dmr_ctx_t *ctx,
                                 int rf, int dst)
{
    char prev[10];

    if (!meta || !ctx)
        return;

    memcpy(prev, meta->net_src, 10);

    if (identity_lookup_dmr_callsign(ctx->aliases, rf, meta->net_src)) {
        LOG_DMR_DEBUG("%s src id %d -> %.10s (subscriber DB)\n",
                      media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), rf, meta->net_src);
    } else {
        identity_format_id_callsign10(meta->net_src, rf);
        LOG_DMR_WARNING("%s src id %d: not in subscriber DB, using numeric\n",
                        media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), rf);
    }

    if (ctx->dmra_text && ctx->dmra_text[0] && ctx->dmra_rf == rf) {
        LOG_DMR_DEBUG("%s id %d DMRA '%s'\n",
                      media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), rf, ctx->dmra_text);
    }

    if (dst > 0)
        identity_format_tg_dst10(meta->net_dst, dst);

    if (memcmp(prev, meta->net_src, 10) != 0) {
        LOG_DMR_INFO("%s talker id %d -> %.10s\n",
                     media_flow_label(MEDIA_PEER_DMR, MEDIA_PEER_YSF), rf, meta->net_src);
    }
}

static int identity_assign_ysf_talker(bridge_call_meta_t *meta, int *ysf_rf_id,
                                      const identity_ysf_ctx_t *ctx, const char *src)
{
    char base[16];
    char talker[10];
    char raw_label[11];
    int id;

    if (!meta || !ysf_rf_id || !ctx)
        return 0;

    identity_dbg_label10(raw_label, (const uint8_t *)(src ? src : (const char *)"          "));
    identity_callsign_base_src(src, base);
    identity_format_base_callsign10(talker, base);
    memcpy(meta->net_src, talker, 10);
    LOG_DMR_DEBUG("%s callsign raw=%s base=%s\n",
                  media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR), raw_label, base);

    id = identity_callsign10_to_dmrid((const uint8_t *)talker);
    if (id <= 0)
        id = identity_lookup_alias_id(ctx->aliases, base);
    if (id > 0) {
        *ysf_rf_id = id;
        LOG_DMR_DEBUG("%s base %s -> id %d (alias)\n",
                      media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR), base, id);
        return 1;
    }

    if (ctx->bridge_dmrid > 0 && ctx->bridge_callsign) {
        memcpy(meta->net_src, ctx->bridge_callsign, 10);
        *ysf_rf_id = ctx->bridge_dmrid;
        LOG_DMR_INFO("%s talker %s unknown -> bridge %.10s id %d\n",
                     media_flow_label(MEDIA_PEER_YSF, MEDIA_PEER_DMR),
                     base, meta->net_src, *ysf_rf_id);
        return 1;
    }
    return 0;
}

static int wire_src_fallback(bridge_call_meta_t *meta, int *ysf_rf_id,
                             const identity_ysf_ctx_t *ctx, const uint8_t *pkt)
{
    char wire[11];

    identity_dbg_label10(wire, pkt + 14);
    return identity_assign_ysf_talker(meta, ysf_rf_id, ctx, wire);
}

int identity_resolve_ysf_header(bridge_call_meta_t *meta, int *ysf_rf_id,
                                const identity_ysf_ctx_t *ctx, const uint8_t *pkt155)
{
    uint8_t rf[120];
    uint8_t scratch[120];
    char csd_src[11], csd_dst[11];
    int i;
    const uint8_t *tries[2];

    if (!meta || !ysf_rf_id || !ctx || !pkt155)
        return 0;

    tries[0] = ysf_tx_modeconv_chunk(pkt155, scratch);
    tries[1] = pkt155 + YSF_FICH_OFFSET_NET;

    for (i = 0; i < 2; i++) {
        memcpy(rf, tries[i], 120);
        if (!ysf_payload_process_header(rf, csd_src, csd_dst))
            continue;

        if (identity_assign_ysf_talker(meta, ysf_rf_id, ctx, csd_src)) {
            LOG_YSF_DEBUG("YSF HEADER CSD src=%.10s dst=%.10s -> DMR id %d (try %d)\n",
                          csd_src, csd_dst, *ysf_rf_id, i);
            return 1;
        }
        LOG_YSF_DEBUG("YSF HEADER CSD src=%.10s dst=%.10s (no DMR id, try %d)\n",
                      csd_src, csd_dst, i);
    }

    if (wire_src_fallback(meta, ysf_rf_id, ctx, pkt155)) {
        LOG_YSF_DEBUG("YSF HEADER wire src=%.10s -> DMR id %d\n", meta->net_src, *ysf_rf_id);
        return 1;
    }

    LOG_YSF_DEBUG("YSF HEADER: no talker DMR id (bridge dmrid not set)\n");
    return 0;
}

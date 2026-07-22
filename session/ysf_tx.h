/*
 * Shared YSFD TX build/send (bridge.c + bridge_el.c).
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_YSF_TX_H
#define ADN_YSF_TX_H

#include <stdint.h>
#include <time.h>

#include "media/call_meta.h"
#include "peer_ysf.h"

#define YSF_FI_HEADER         0x00U
#define YSF_FI_COMMUNICATIONS 0x01U
#define YSF_FI_TERMINATOR     0x02U
#define YSF_FICH_FT           6U
#define YSF_FICH_CM           0U
#define YSF_DT_VD_MODE2       0x02U
#define YSF_WIRE_DST_ALL      "ALL       "

typedef struct {
    peer_ysf_t *peer;
    const char *repeater_callsign; /* peer->callsign, 10 chars */
    const bridge_call_meta_t *meta;
    struct timespec *last_tx;
    uint8_t *ysf_fn;
    unsigned dgid_cfg;
} ysf_tx_args_t;

const char *ysf_tx_fi_name(uint8_t fi);
const uint8_t *ysf_tx_modeconv_chunk(const uint8_t *pkt155, uint8_t scratch[120]);
void ysf_tx_fill_csd(const bridge_call_meta_t *meta, uint8_t csd1[20], uint8_t csd2[20]);
void ysf_tx_apply_dch_slot(uint8_t *payload, uint8_t fn, const bridge_call_meta_t *meta);
int ysf_tx_send(ysf_tx_args_t *args, uint8_t fi, uint8_t ft, uint8_t cm,
                uint8_t fich_fn, uint8_t net_cnt, const uint8_t *payload120,
                const uint8_t csd1[20], const uint8_t csd2[20]);

#endif

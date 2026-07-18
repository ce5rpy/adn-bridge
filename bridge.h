/*
 * YSF <-> DMR voice bridge.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef YSF2DMR_BRIDGE_H
#define YSF2DMR_BRIDGE_H

#include <stdint.h>
#include <time.h>
#include "aliases.h"
#include "peer_dmr.h"
#include "peer_ysf.h"

typedef struct {
    peer_dmr_t dmr;
    peer_ysf_t ysf;
    ysf2dmr_aliases_t *aliases;
    int default_ysf_dmrid;
    uint32_t dmr_stream_id;
    uint8_t dmr_seq;
    uint8_t dmr_slot_bit;  /* always 0x80 = TS2 */
    uint8_t ysf_fn;
    uint8_t ysf_cnt;
    char net_src[10];
    char net_dst[10];
    int ysf_rf_id;
    int dmr_voice_frames;
    int dmr_tx_frames;
    int dmr_dmrd_other;
    int ysf_voice_frames;
    int call_active;
    uint8_t dmr_last_dtype; /* YSF2DMR m_dmrLastDT — gate putDMRHeader */
    struct {
        int rf;
        uint8_t blocks[4][7];
        unsigned have;
        char text[32];
    } dmra;
    struct timespec last_dmr_tx;
    struct timespec last_ysf_tx;
    /* Rising-edge PTT on DMR login: 500 ms silence to [dmr] tg (optional 4000 first) */
    int dmr_was_connected;
    int clear_dynamic_tg;
    int connect_ptt_active;
    int connect_ptt_phase; /* 0=need VHEAD, 1=voice, 2=need VTERM */
    int connect_ptt_voice_frames;
    int connect_ptt_tg;
    int connect_ptt_clearing;
    struct timespec connect_ptt_start;
} ysf2dmr_bridge_t;

void bridge_init(ysf2dmr_bridge_t *b, const char *dmr_options,
                 ysf2dmr_aliases_t *aliases, int default_ysf_dmrid,
                 int clear_dynamic_tg);
void bridge_on_dmrd(ysf2dmr_bridge_t *b, const uint8_t *pkt, int len);
void bridge_on_dmra(ysf2dmr_bridge_t *b, const uint8_t *pkt, int len);
void bridge_on_ysfd(ysf2dmr_bridge_t *b, const uint8_t *pkt, int len);
void bridge_tick(ysf2dmr_bridge_t *b);
/* Abort connect-PTT early (e.g. real YSF call starts). */
void bridge_abort_connect_ptt(ysf2dmr_bridge_t *b);

#endif

/*
 * DMR LC/header codec exports for bridge TX.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * Copyright (C) 2025 Esteban Mackay, HP3ICC
 * Copyright (C) 2019 Doug McLain
 * Based on code from reflector_connectors/dmrcon.c and MMDVM_CM
 *
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

#ifndef DMR_CODEC_H
#define DMR_CODEC_H

#include <stdbool.h>
#include <stdint.h>

extern uint8_t buf[];
extern int tx_tgid;
extern int rx_srcid;

void generate_header(void);
/* emb_raw is 128 bools owned by the caller (media_peer_slot_t.dmr_emb_raw),
 * not a shared global -- it must survive from the n=0 encode to the n=1..5
 * reads that follow across separate calls. */
void encode_embedded_data(bool *emb_raw_out);
uint8_t get_embedded_data(uint8_t *data, uint8_t n, const bool *emb_raw_in);
void get_emb_data(uint8_t *data, uint8_t lcss);

#endif

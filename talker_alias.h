/*
 * DMR Talker Alias (HBP DMRA) decode for ysf2dmrcon.
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

#ifndef TALKER_ALIAS_H
#define TALKER_ALIAS_H

#include <stddef.h>
#include <stdint.h>

#define DMRA_PACKET_LEN 15

/* Parse 15-byte HBP DMRA; returns 1 on success. */
int dmra_parse_packet(const uint8_t *data, int len, int *rf_out, int *block_id,
                      uint8_t payload7[7]);

/* Merge up to four 7-byte blocks (mask bit i = block i present) into talker text. */
int dmra_decode_blocks(const uint8_t blocks[4][7], unsigned have_mask,
                       char *text, size_t textlen);

#endif

/*
 * DMRD wire constants and RX classification helpers.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADN_DMR_WIRE_H
#define ADN_DMR_WIRE_H

#include <stdint.h>

#define DMRD_FT_DATA_SYNC  2U
#define DMRD_FT_VOICE_SYNC 1U
#define DMRD_DTYPE_VHEAD   1U
#define DMRD_DTYPE_VTERM   2U
#define DMRD_CALL_PRIVATE  0x40U /* byte 15 bit 6: unit (private) call, not group */

extern const uint8_t DMR_MS_SOURCED_AUDIO_SYNC[7];
extern const uint8_t DMR_SYNC_MASK[7];
extern const uint8_t DMR_SILENCE_DATA[33];

void dmrd_parse_b15(uint8_t b15, uint8_t *ft, uint8_t *dtype);
int dmrd_is_header(const uint8_t *pkt, int len);
int dmrd_is_terminator(const uint8_t *pkt, int len);
int dmrd_is_voice(const uint8_t *pkt, int len);
const char *dmrd_class_label(const uint8_t *pkt, int len);

int dmr_id_rf24(int dmrid);
void dmr_id_to_bytes3(int dmrid, uint8_t out[3]);

/* Homebrew control opcode at the start of a UDP payload. */
const char *hbp_cmd_label(const uint8_t *pkt, int len);
/* 4-byte big-endian radio ID immediately after opcode, when len allows. */
int hbp_wire_tail_id(const uint8_t *pkt, int len, const char *cmd, uint32_t *out_id);
/* Lowercase hex dump (truncates with "..." when capped). */
void hbp_wire_hex(const uint8_t *pkt, int len, char *out, int out_cap);

#endif

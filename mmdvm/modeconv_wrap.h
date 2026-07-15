/*
 * C wrapper around MMDVM_CM CModeConv.
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

#ifndef MODECONV_WRAP_H
#define MODECONV_WRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODECONV_TAG_NODATA 0x04U
#define MODECONV_TAG_HEADER 0x00U
#define MODECONV_TAG_DATA   0x01U
#define MODECONV_TAG_EOT    0x03U

void modeconv_init(void);
void modeconv_reset(void);

void modeconv_put_dmr_voice(const uint8_t *frame33);
void modeconv_put_dmr_header(void);
void modeconv_put_dmr_eot(void);

void modeconv_put_ysf_payload(const uint8_t *ysf120);
void modeconv_put_ysf_header(void);
void modeconv_put_ysf_eot(void);

/* Returns tag (MODECONV_TAG_*). voice33 must hold 33 bytes for TAG_DATA. */
unsigned int modeconv_get_dmr(uint8_t *voice33);
/* ysf120 is the 120-byte YSF payload (sync..); returns tag. */
unsigned int modeconv_get_ysf(uint8_t *ysf120);

#ifdef __cplusplus
}
#endif

#endif

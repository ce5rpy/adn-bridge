/*
 * C wrapper around MMDVM_CM CYSFPayload.
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

#ifndef YSF2DMR_YSFPAYLOAD_WRAP_H
#define YSF2DMR_YSFPAYLOAD_WRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ysf_payload_write_header(uint8_t *payload120,
                              const uint8_t csd1[20], const uint8_t csd2[20]);
void ysf_payload_write_vd_mode2_dch(uint8_t *payload120, const uint8_t dch[10]);
/* YSF2DMR: processHeaderData + getSource/getDest; src11/dst11 are 11-byte NUL-terminated. */
int ysf_payload_process_header(uint8_t *payload120, char src11[11], char dst11[11]);

#ifdef __cplusplus
}
#endif

#endif

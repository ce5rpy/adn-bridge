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

/* Opaque per-instance ring-buffer state. Each concurrent cross-kind pairing
 * (DMR<->YSF direct; EchoLink<->DMR/YSF vocoder) needs its own instance —
 * they must never share one, or two calls happening in the same tick (a
 * 3+-kind bus with a DMR source reaching both a YSF and an EchoLink
 * destination) would corrupt each other's ring buffer. media_core_t holds
 * one instance per pairing (mc_ysf_dmr, mc_el); create with modeconv_create(). */
typedef struct modeconv_s modeconv_t;

modeconv_t *modeconv_create(void);
void modeconv_reset(modeconv_t *m);

void modeconv_put_dmr_voice(modeconv_t *m, const uint8_t *frame33);
void modeconv_put_dmr_header(modeconv_t *m);
void modeconv_put_dmr_eot(modeconv_t *m);

void modeconv_put_ysf_payload(modeconv_t *m, const uint8_t *ysf120);
void modeconv_put_ysf_header(modeconv_t *m);
void modeconv_put_ysf_eot(modeconv_t *m);

/* Returns tag (MODECONV_TAG_*). voice33 must hold 33 bytes for TAG_DATA. */
unsigned int modeconv_get_dmr(modeconv_t *m, uint8_t *voice33);
/* ysf120 is the 120-byte YSF payload (sync..); returns tag. */
unsigned int modeconv_get_ysf(modeconv_t *m, uint8_t *ysf120);

/* Queue one 7-byte AMBE frame into ModeConv (3 needed for one DMR 33-byte frame). */
void modeconv_put_ambe7(modeconv_t *m, const uint8_t ambe7[7]);
/* Queue one 7-byte AMBE into YSF ModeConv path (5 needed for one YSF payload). */
void modeconv_put_ambe7_ysf(modeconv_t *m, const uint8_t ambe7[7]);
/* Extract three 7-byte AMBE frames from a 33-byte DMR voice burst — stateless,
 * no instance needed (does not touch any CModeConv member). */
void modeconv_dmr33_to_ambe(const uint8_t dmr33[33], uint8_t ambe[3][7]);

#ifdef __cplusplus
}
#endif

#endif

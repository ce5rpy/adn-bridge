/*
 * YSF FICH codec and DGID helpers.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * Copyright (C) 2019 Doug McLain
 * Copyright (C) 2025 Esteban Mackay, HP3ICC
 * Derived from reflector_connectors/dgidcon.c
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

#ifndef YSF_FICH_H
#define YSF_FICH_H

#include <stdint.h>
#include <netinet/in.h>

#define YSF_FICH_DT_VD_MODE2 2U
/* Network YSFD: 120-byte RF chunk at frame+35 (sync+FICH+VCH), same as YSF2DMR. */
#define YSF_FICH_OFFSET_NET 35U
/* FICH position on the wire: after the 5-byte sync. */
#define YSF_FICH_OFFSET_RX  40U

void ysf_send_activation_burst(int udp_sock, const struct sockaddr_in *host,
                               const char callsign[10], uint8_t forced_dgid);

/* Rewrite FICH DGID only; voice/GPS payload (bytes 55+) unchanged. */
void ysf_fich_rewrite_dgid(uint8_t *frame155, uint8_t dgid);

/* Decode inbound YSFD FICH (auto +35 network / +40 RF); returns 0 on success. */
int ysf_fich_decode_fields(const uint8_t *frame155, uint8_t *fi, uint8_t *fn,
                           uint8_t *ft, uint8_t *cm, uint8_t *dt);
/* DGID from last ysf_fich_decode_fields / fich_decode (m_fich[3]). */
uint8_t ysf_fich_get_dgid(void);

/* Base voice FICH (SQL/SQ cleared); peer_ysf_send_ysfd applies config DGID. */
void ysf_fich_encode_outbound(uint8_t *fich25, uint8_t fn_serial,
                              uint8_t fi, uint8_t ft, uint8_t cm);

#endif

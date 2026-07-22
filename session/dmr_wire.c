/*
 * DMRD wire constants and RX classification helpers.
 *
 * Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session/dmr_wire.h"

#include <string.h>

const uint8_t DMR_MS_SOURCED_AUDIO_SYNC[7] =
    {0x07U, 0xF7U, 0xD5U, 0xDDU, 0x57U, 0xDFU, 0xD0U};
const uint8_t DMR_SYNC_MASK[7] =
    {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U};
const uint8_t DMR_SILENCE_DATA[33] =
    {0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU, 0xB9U, 0xE8U,
     0x81U, 0x52U, 0x60U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x73U, 0x00U,
     0x2AU, 0x6BU, 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU};

void dmrd_parse_b15(uint8_t b15, uint8_t *ft, uint8_t *dtype)
{
    *ft = (b15 >> 4) & 0x03U;
    *dtype = b15 & 0x0fU;
}

int dmrd_is_header(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    if (pkt[15] & 0x40)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft == DMRD_FT_DATA_SYNC && dtype == DMRD_DTYPE_VHEAD;
}

int dmrd_is_terminator(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft == DMRD_FT_DATA_SYNC && dtype == DMRD_DTYPE_VTERM;
}

int dmrd_is_voice(const uint8_t *pkt, int len)
{
    uint8_t ft, dtype;

    if (len != 55 || memcmp(pkt, "DMRD", 4) != 0)
        return 0;
    if (pkt[15] & 0x40)
        return 0;
    dmrd_parse_b15(pkt[15], &ft, &dtype);
    return ft <= 1U;
}

const char *dmrd_class_label(const uint8_t *pkt, int len)
{
    if (dmrd_is_header(pkt, len))
        return "VHEAD";
    if (dmrd_is_terminator(pkt, len))
        return "VTERM";
    if (dmrd_is_voice(pkt, len))
        return "VOICE";
    return "OTHER";
}

int dmr_id_rf24(int dmrid)
{
    return (dmrid > 99999999) ? dmrid / 100 : dmrid;
}

void dmr_id_to_bytes3(int dmrid, uint8_t out[3])
{
    int id = dmr_id_rf24(dmrid);

    out[0] = (uint8_t)((id >> 16) & 0xff);
    out[1] = (uint8_t)((id >> 8) & 0xff);
    out[2] = (uint8_t)(id & 0xff);
}

/*
 * DMR Talker Alias (HBP DMRA) decode — port of ADN talker_alias decode path.
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

#include "talker_alias.h"

#include <string.h>

#define TA_BUF_LEN 28U
#define TA_PAYLOAD_LEN 7U

static int decode_ta_utf(const uint8_t *buf, char *text, size_t textlen)
{
    unsigned ta_format, ta_size, i;

    if (!buf || !text || textlen == 0)
        return 0;
    ta_format = (buf[0] >> 6) & 0x03U;
    ta_size = (buf[0] >> 1) & 0x1FU;
    if (ta_size == 0 || ta_size > 29U)
        return 0;
    if (ta_format != 1U && ta_format != 2U)
        return 0;
    if (ta_size >= textlen)
        ta_size = (unsigned)textlen - 1U;
    for (i = 0; i < ta_size; i++)
        text[i] = (char)buf[1U + i];
    text[ta_size] = '\0';
    return (int)ta_size;
}

static int decode_ta_7bit(const uint8_t *buf, char *text, size_t textlen)
{
    unsigned ta_size, t1 = 0, t2 = 0, c = 0, outlen = 0;
    size_t i;
    int j;

    if (!buf || !text || textlen == 0)
        return 0;
    if (((buf[0] >> 6) & 0x03U) != 0U)
        return 0;
    ta_size = (buf[0] >> 1) & 0x1FU;
    if (ta_size == 0 || ta_size >= textlen)
        return 0;

    for (i = 0; i < 32U && i < TA_BUF_LEN; i++) {
        if (t2 >= ta_size)
            break;
        for (j = 7; j >= 0; j--) {
            c = ((c << 1) | ((buf[i] >> (unsigned)j) & 1U)) & 0xFFU;
            t1++;
            if (t1 == 7U) {
                if (i > 0U && outlen < ta_size) {
                    text[outlen++] = (char)(c & 0x7FU);
                    t2++;
                }
                t1 = 0;
                c = 0;
            }
        }
    }
    text[outlen] = '\0';
    return outlen > 0 ? (int)outlen : 0;
}

static int decode_ta_buf(const uint8_t *buf, char *text, size_t textlen)
{
    unsigned fmt;

    if (!buf || !text || textlen == 0)
        return 0;
    fmt = (buf[0] >> 6) & 0x03U;
    if (fmt == 1U || fmt == 2U)
        return decode_ta_utf(buf, text, textlen);
    if (fmt == 0U)
        return decode_ta_7bit(buf, text, textlen);
    return 0;
}

static void trim_ta_text(char *text)
{
    size_t n, i;

    if (!text)
        return;
    n = strlen(text);
    while (n > 0 && (text[n - 1] == '\0' || text[n - 1] == ' '))
        text[--n] = '\0';
    for (i = 0; text[i]; i++) {
        if (text[i] < 32 || text[i] == 127) {
            text[i] = '\0';
            break;
        }
    }
}

int dmra_parse_packet(const uint8_t *data, int len, int *rf_out, int *block_id,
                      uint8_t payload7[7])
{
    if (!data || len < DMRA_PACKET_LEN || memcmp(data, "DMRA", 4) != 0)
        return 0;
    if (data[7] > 3U)
        return 0;
    if (rf_out)
        *rf_out = ((int)data[4] << 16) | ((int)data[5] << 8) | (int)data[6];
    if (block_id)
        *block_id = (int)data[7];
    if (payload7)
        memcpy(payload7, data + 8, TA_PAYLOAD_LEN);
    return 1;
}

int dmra_decode_blocks(const uint8_t blocks[4][7], unsigned have_mask,
                       char *text, size_t textlen)
{
    uint8_t buf[TA_BUF_LEN];
    unsigned i;

    if (!blocks || !text || textlen == 0 || (have_mask & 1U) == 0)
        return 0;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < 4U; i++) {
        if (have_mask & (1U << i))
            memcpy(buf + i * TA_PAYLOAD_LEN, blocks[i], TA_PAYLOAD_LEN);
    }

    if (!decode_ta_buf(buf, text, textlen))
        return 0;
    trim_ta_text(text);
    return text[0] ? 1 : 0;
}

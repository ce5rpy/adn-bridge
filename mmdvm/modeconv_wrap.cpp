/*
 * C wrapper around MMDVM_CM CModeConv for ysf2dmrcon.
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

#include "modeconv_wrap.h"

#include "ModeConv.h"
#include <new>

static CModeConv g_conv;

extern "C" void modeconv_init(void)
{
}

extern "C" void modeconv_reset(void)
{
    /* CRingBuffer has no copy assignment; g_conv = CModeConv() would
     * shallow-copy buffer pointers and double-free on the temporary dtor. */
    g_conv.~CModeConv();
    new (&g_conv) CModeConv();
}

extern "C" void modeconv_put_dmr_voice(const uint8_t *frame33)
{
    g_conv.putDMR(const_cast<unsigned char *>(frame33));
}

extern "C" void modeconv_put_dmr_header(void)
{
    g_conv.putDMRHeader();
}

extern "C" void modeconv_put_dmr_eot(void)
{
    g_conv.putDMREOT();
}

extern "C" void modeconv_put_ysf_payload(const uint8_t *ysf120)
{
    g_conv.putYSF(const_cast<unsigned char *>(ysf120));
}

extern "C" void modeconv_put_ysf_header(void)
{
    g_conv.putYSFHeader();
}

extern "C" void modeconv_put_ysf_eot(void)
{
    g_conv.putYSFEOT();
}

extern "C" unsigned int modeconv_get_dmr(uint8_t *voice33)
{
    return g_conv.getDMR(voice33);
}

extern "C" unsigned int modeconv_get_ysf(uint8_t *ysf120)
{
    return g_conv.getYSF(ysf120);
}

extern "C" void modeconv_put_ambe7(const uint8_t ambe7[7])
{
    g_conv.putAMBE7(ambe7);
}

extern "C" void modeconv_put_ambe7_ysf(const uint8_t ambe7[7])
{
    g_conv.putAMBE7YSF(ambe7);
}

extern "C" void modeconv_dmr33_to_ambe(const uint8_t dmr33[33], uint8_t ambe[3][7])
{
    g_conv.dmr33ToAMBE(dmr33, ambe);
}

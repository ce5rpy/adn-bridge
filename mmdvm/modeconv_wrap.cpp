/*
 * C wrapper around MMDVM_CM CModeConv for adn-bridge.
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

struct modeconv_s {
    CModeConv conv;
};

extern "C" modeconv_t *modeconv_create(void)
{
    return new modeconv_s();
}

extern "C" void modeconv_reset(modeconv_t *m)
{
    if (!m)
        return;
    /* CRingBuffer has no copy assignment; m->conv = CModeConv() would
     * shallow-copy buffer pointers and double-free on the temporary dtor. */
    m->conv.~CModeConv();
    new (&m->conv) CModeConv();
}

extern "C" void modeconv_put_dmr_voice(modeconv_t *m, const uint8_t *frame33)
{
    if (m)
        m->conv.putDMR(const_cast<unsigned char *>(frame33));
}

extern "C" void modeconv_put_dmr_header(modeconv_t *m)
{
    if (m)
        m->conv.putDMRHeader();
}

extern "C" void modeconv_put_dmr_eot(modeconv_t *m)
{
    if (m)
        m->conv.putDMREOT();
}

extern "C" void modeconv_put_ysf_payload(modeconv_t *m, const uint8_t *ysf120)
{
    if (m)
        m->conv.putYSF(const_cast<unsigned char *>(ysf120));
}

extern "C" void modeconv_put_ysf_header(modeconv_t *m)
{
    if (m)
        m->conv.putYSFHeader();
}

extern "C" void modeconv_put_ysf_eot(modeconv_t *m)
{
    if (m)
        m->conv.putYSFEOT();
}

extern "C" unsigned int modeconv_get_dmr(modeconv_t *m, uint8_t *voice33)
{
    return m ? m->conv.getDMR(voice33) : MODECONV_TAG_NODATA;
}

extern "C" unsigned int modeconv_get_ysf(modeconv_t *m, uint8_t *ysf120)
{
    return m ? m->conv.getYSF(ysf120) : MODECONV_TAG_NODATA;
}

extern "C" void modeconv_put_ambe7(modeconv_t *m, const uint8_t ambe7[7])
{
    if (m)
        m->conv.putAMBE7(ambe7);
}

extern "C" void modeconv_put_ambe7_ysf(modeconv_t *m, const uint8_t ambe7[7])
{
    if (m)
        m->conv.putAMBE7YSF(ambe7);
}

/* Stateless (pure bit extraction, no CModeConv member touched) — one shared
 * instance is safe across every caller, unlike the ring-buffer API above. */
extern "C" void modeconv_dmr33_to_ambe(const uint8_t dmr33[33], uint8_t ambe[3][7])
{
    static CModeConv stateless;
    stateless.dmr33ToAMBE(dmr33, ambe);
}

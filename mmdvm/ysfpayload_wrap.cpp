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

#include "ysfpayload_wrap.h"

#include "YSFPayload.h"

#include <cstring>
#include <string>

extern "C" void ysf_payload_write_header(uint8_t *payload120,
                                         const uint8_t csd1[20],
                                         const uint8_t csd2[20])
{
	CYSFPayload payload;
	payload.writeHeader(payload120, csd1, csd2);
}

extern "C" void ysf_payload_write_vd_mode2_dch(uint8_t *payload120,
                                             const uint8_t dch[10])
{
	CYSFPayload payload;
	payload.writeVDMode2Data(payload120, dch);
}

extern "C" int ysf_payload_process_header(uint8_t *payload120, char src11[11],
                                          char dst11[11])
{
	CYSFPayload payload;
	std::string src;
	std::string dst;
	size_t n;

	if (payload120 == NULL || src11 == NULL || dst11 == NULL)
		return 0;

	memset(src11, 0, 11);
	memset(dst11, 0, 11);
	if (!payload.processHeaderData(payload120))
		return 0;

	src = payload.getSource();
	dst = payload.getDest();

	n = src.size() < 10U ? src.size() : 10U;
	if (n > 0U)
		memcpy(src11, src.c_str(), n);
	n = dst.size() < 10U ? dst.size() : 10U;
	if (n > 0U)
		memcpy(dst11, dst.c_str(), n);
	return 1;
}

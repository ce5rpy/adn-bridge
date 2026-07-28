/*
 *   Copyright (C) 2009,2014,2015,2016 Jonathan Naylor, G4KLX
 *
 *   Stub implementation for adn-bridge (vendored from MMDVM_CM YSF2DMR).
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "Utils.h"

void CUtils::dump(const std::string& title, const unsigned char* data, unsigned int length)
{
	(void)title;
	(void)data;
	(void)length;
}

void CUtils::dump(int level, const std::string& title, const unsigned char* data, unsigned int length)
{
	(void)level;
	(void)title;
	(void)data;
	(void)length;
}

void CUtils::dump(const std::string& title, const bool* bits, unsigned int length)
{
	(void)title;
	(void)bits;
	(void)length;
}

void CUtils::dump(int level, const std::string& title, const bool* bits, unsigned int length)
{
	(void)level;
	(void)title;
	(void)bits;
	(void)length;
}

void CUtils::byteToBitsBE(unsigned char byte, bool* bits)
{
	for (unsigned int i = 0U; i < 8U; i++)
		bits[i] = (byte & (1U << (7U - i))) != 0U;
}

void CUtils::byteToBitsLE(unsigned char byte, bool* bits)
{
	for (unsigned int i = 0U; i < 8U; i++)
		bits[i] = (byte & (1U << i)) != 0U;
}

void CUtils::bitsToByteBE(const bool* bits, unsigned char& byte)
{
	byte = 0U;
	for (unsigned int i = 0U; i < 8U; i++)
		if (bits[i])
			byte |= 1U << (7U - i);
}

void CUtils::bitsToByteLE(const bool* bits, unsigned char& byte)
{
	byte = 0U;
	for (unsigned int i = 0U; i < 8U; i++)
		if (bits[i])
			byte |= 1U << i;
}

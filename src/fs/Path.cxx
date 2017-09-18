/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Path.hxx"
#include "Charset.hxx"

#include <stdexcept>

std::string
Path::ToUTF8() const noexcept
{
	try {
		return ::PathToUTF8(c_str());
	} catch (const std::runtime_error &) {
		return std::string();
	}
}

Path::const_pointer_type
Path::GetSuffix() const noexcept
{
	const auto base = GetBase().c_str();
	const auto *dot = StringFindLast(base, '.');
	if (dot == nullptr || dot == base)
		return nullptr;

	return dot + 1;
}

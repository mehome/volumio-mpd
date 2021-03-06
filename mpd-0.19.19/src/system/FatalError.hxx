/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
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

#ifndef MPD_FATAL_ERROR_HXX
#define MPD_FATAL_ERROR_HXX

#include "check.h"
#include "Compiler.h"

class Error;

/**
 * Log the specified message and abort the process.
 */
gcc_noreturn
void
FatalError(const char *msg);

gcc_noreturn
void
FormatFatalError(const char *fmt, ...);

gcc_noreturn
void
FatalError(const Error &error);

gcc_noreturn
void
FatalError(const char *msg, const Error &error);

/**
 * Call this after a system call has failed that is not supposed to
 * fail.  Prints the given message, the system error message (from
 * errno or GetLastError()) and abort the process.
 */
gcc_noreturn
void
FatalSystemError(const char *msg);

gcc_noreturn
void
FormatFatalSystemError(const char *fmt, ...);

#endif

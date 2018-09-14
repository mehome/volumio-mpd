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
#include "BufferedSocket.hxx"
#include "net/SocketError.hxx"
#include "Compiler.h"

#include <algorithm>

BufferedSocket::ssize_t
BufferedSocket::DirectRead(void *data, size_t length)
{
	const auto nbytes = SocketMonitor::Read((char *)data, length);
	if (gcc_likely(nbytes > 0))
		return nbytes;

	if (nbytes == 0) {
		OnSocketClosed();
		return -1;
	}

	const auto code = GetSocketError();
	if (IsSocketErrorAgain(code))
		return 0;

	if (IsSocketErrorClosed(code))
		OnSocketClosed();
	else
		OnSocketError(std::make_exception_ptr(MakeSocketError(code, "Failed to receive from socket")));
	return -1;
}

bool
BufferedSocket::ReadToBuffer()
{
	assert(IsDefined());

	const auto buffer = input.Write();
	assert(!buffer.IsEmpty());

	const auto nbytes = DirectRead(buffer.data, buffer.size);
	if (nbytes > 0)
		input.Append(nbytes);

	return nbytes >= 0;
}

bool
BufferedSocket::ResumeInput()
{
	assert(IsDefined());

	while (true) {
		const auto buffer = input.Read();
		if (buffer.IsEmpty()) {
			ScheduleRead();
			return true;
		}

		const auto result = OnSocketInput(buffer.data, buffer.size);
		switch (result) {
		case InputResult::MORE:
			if (input.IsFull()) {
				OnSocketError(std::make_exception_ptr(std::runtime_error("Input buffer is full")));
				return false;
			}

			ScheduleRead();
			return true;

		case InputResult::PAUSE:
			CancelRead();
			return true;

		case InputResult::AGAIN:
			continue;

		case InputResult::CLOSED:
			return false;
		}
	}
}

bool
BufferedSocket::OnSocketReady(unsigned flags)
{
	assert(IsDefined());

	if (gcc_unlikely(flags & (ERROR|HANGUP))) {
		OnSocketClosed();
		return false;
	}

	if (flags & READ) {
		assert(!input.IsFull());

		if (!ReadToBuffer())
			return false;

		if (!ResumeInput())
			/* we must return "true" here or
			   SocketMonitor::Dispatch() will call
			   Cancel() on a freed object */
			return true;

		if (!input.IsFull())
			ScheduleRead();
	}

	return true;
}

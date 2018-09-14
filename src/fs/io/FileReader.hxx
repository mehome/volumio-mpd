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

#ifndef MPD_FILE_READER_HXX
#define MPD_FILE_READER_HXX

#include "check.h"
#include "Reader.hxx"
#include "fs/AllocatedPath.hxx"
#include "Compiler.h"

#ifndef _WIN32
#include "system/FileDescriptor.hxx"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

class Path;
class FileInfo;

class FileReader final : public Reader {
	AllocatedPath path;

#ifdef _WIN32
	HANDLE handle;
#else
	FileDescriptor fd;
#endif

public:
	explicit FileReader(Path _path);

#ifdef _WIN32
	FileReader(FileReader &&other)
		:path(std::move(other.path)),
		 handle(other.handle) {
		other.handle = INVALID_HANDLE_VALUE;
	}
#else
	FileReader(FileReader &&other)
		:path(std::move(other.path)),
		 fd(other.fd) {
		other.fd.SetUndefined();
	}
#endif

	~FileReader() {
		if (IsDefined())
			Close();
	}


protected:
	bool IsDefined() const {
#ifdef _WIN32
		return handle != INVALID_HANDLE_VALUE;
#else
		return fd.IsDefined();
#endif
	}

public:
#ifndef _WIN32
	FileDescriptor GetFD() const {
		return fd;
	}
#endif

	void Close();

	FileInfo GetFileInfo() const;

	gcc_pure
	uint64_t GetSize() const noexcept {
#ifdef _WIN32
		LARGE_INTEGER size;
		return GetFileSizeEx(handle, &size)
			? size.QuadPart
			: 0;
#else
		return fd.GetSize();
#endif
	}

	gcc_pure
	uint64_t GetPosition() const noexcept {
#ifdef _WIN32
		LARGE_INTEGER zero;
		zero.QuadPart = 0;
		LARGE_INTEGER position;
		return SetFilePointerEx(handle, zero, &position, FILE_CURRENT)
			? position.QuadPart
			: 0;
#else
		return fd.Tell();
#endif
	}

	void Rewind() {
		Seek(0);
	}

	void Seek(off_t offset);
	void Skip(off_t offset);

	/* virtual methods from class Reader */
	size_t Read(void *data, size_t size) override;
};

#endif

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

/**
  * single bz2 archive handling (requires libbz2)
  */

#include "config.h"
#include "Bzip2ArchivePlugin.hxx"
#include "../ArchivePlugin.hxx"
#include "../ArchiveFile.hxx"
#include "../ArchiveVisitor.hxx"
#include "input/InputStream.hxx"
#include "input/LocalOpen.hxx"
#include "thread/Cond.hxx"
#include "util/RefCount.hxx"
#include "fs/Path.hxx"

#include <bzlib.h>

#include <stdexcept>

#include <stddef.h>

class Bzip2ArchiveFile final : public ArchiveFile {
public:
	RefCount ref;

	std::string name;
	const InputStreamPtr istream;

	Bzip2ArchiveFile(Path path, InputStreamPtr &&_is)
		:ArchiveFile(bz2_archive_plugin),
		 name(path.GetBase().c_str()),
		 istream(std::move(_is)) {
		// remove .bz2 suffix
		const size_t len = name.length();
		if (len > 4)
			name.erase(len - 4);
	}

	void Ref() {
		ref.Increment();
	}

	void Unref() {
		if (!ref.Decrement())
			return;

		delete this;
	}

	virtual void Close() override {
		Unref();
	}

	virtual void Visit(ArchiveVisitor &visitor) override {
		visitor.VisitArchiveEntry(name.c_str());
	}

	InputStream *OpenStream(const char *path,
				Mutex &mutex, Cond &cond) override;
};

class Bzip2InputStream final : public InputStream {
	Bzip2ArchiveFile *archive;

	bool eof = false;

	bz_stream bzstream;

	char buffer[5000];

public:
	Bzip2InputStream(Bzip2ArchiveFile &context, const char *uri,
			 Mutex &mutex, Cond &cond);
	~Bzip2InputStream();

	/* virtual methods from InputStream */
	bool IsEOF() noexcept override;
	size_t Read(void *ptr, size_t size) override;

private:
	void Open();
	bool FillBuffer();
};

/* single archive handling allocation helpers */

inline void
Bzip2InputStream::Open()
{
	bzstream.bzalloc = nullptr;
	bzstream.bzfree = nullptr;
	bzstream.opaque = nullptr;

	bzstream.next_in = (char *)buffer;
	bzstream.avail_in = 0;

	int ret = BZ2_bzDecompressInit(&bzstream, 0, 0);
	if (ret != BZ_OK)
		throw std::runtime_error("BZ2_bzDecompressInit() has failed");

	SetReady();
}

/* archive open && listing routine */

static ArchiveFile *
bz2_open(Path pathname)
{
	static Mutex mutex;
	static Cond cond;
	auto is = OpenLocalInputStream(pathname, mutex, cond);
	return new Bzip2ArchiveFile(pathname, std::move(is));
}

/* single archive handling */

Bzip2InputStream::Bzip2InputStream(Bzip2ArchiveFile &_context,
				   const char *_uri,
				   Mutex &_mutex, Cond &_cond)
	:InputStream(_uri, _mutex, _cond),
	 archive(&_context)
{
	Open();
	archive->Ref();
}

Bzip2InputStream::~Bzip2InputStream()
{
	BZ2_bzDecompressEnd(&bzstream);
	archive->Unref();
}

InputStream *
Bzip2ArchiveFile::OpenStream(const char *path,
			     Mutex &mutex, Cond &cond)
{
	return new Bzip2InputStream(*this, path, mutex, cond);
}

inline bool
Bzip2InputStream::FillBuffer()
{
	if (bzstream.avail_in > 0)
		return true;

	size_t count = archive->istream->LockRead(buffer, sizeof(buffer));
	if (count == 0)
		return false;

	bzstream.next_in = buffer;
	bzstream.avail_in = count;
	return true;
}

size_t
Bzip2InputStream::Read(void *ptr, size_t length)
{
	const ScopeUnlock unlock(mutex);

	int bz_result;
	size_t nbytes = 0;

	if (eof)
		return 0;

	bzstream.next_out = (char *)ptr;
	bzstream.avail_out = length;

	do {
		if (!FillBuffer())
			return 0;

		bz_result = BZ2_bzDecompress(&bzstream);

		if (bz_result == BZ_STREAM_END) {
			eof = true;
			break;
		}

		if (bz_result != BZ_OK)
			throw std::runtime_error("BZ2_bzDecompress() has failed");
	} while (bzstream.avail_out == length);

	nbytes = length - bzstream.avail_out;
	offset += nbytes;

	return nbytes;
}

bool
Bzip2InputStream::IsEOF() noexcept
{
	return eof;
}

/* exported structures */

static const char *const bz2_extensions[] = {
	"bz2",
	nullptr
};

const ArchivePlugin bz2_archive_plugin = {
	"bz2",
	nullptr,
	nullptr,
	bz2_open,
	bz2_extensions,
};

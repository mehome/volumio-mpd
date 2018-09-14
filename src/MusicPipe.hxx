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

#ifndef MPD_PIPE_H
#define MPD_PIPE_H

#include "thread/Mutex.hxx"
#include "Compiler.h"

#ifndef NDEBUG
#include "AudioFormat.hxx"
#endif

#include <assert.h>

struct MusicChunk;
class MusicBuffer;

/**
 * A queue of #MusicChunk objects.  One party appends chunks at the
 * tail, and the other consumes them from the head.
 */
class MusicPipe {
	/** the first chunk */
	MusicChunk *head = nullptr;

	/** a pointer to the tail of the chunk */
	MusicChunk **tail_r = &head;

	/** the current number of chunks */
	unsigned size = 0;

	/** a mutex which protects #head and #tail_r */
	mutable Mutex mutex;

#ifndef NDEBUG
	AudioFormat audio_format = AudioFormat::Undefined();
#endif

public:
	/**
	 * Creates a new #MusicPipe object.  It is empty.
	 */
	MusicPipe() = default;

	MusicPipe(const MusicPipe &) = delete;

	/**
	 * Frees the object.  It must be empty now.
	 */
	~MusicPipe() {
		assert(head == nullptr);
		assert(tail_r == &head);
	}

	MusicPipe &operator=(const MusicPipe &) = delete;

#ifndef NDEBUG
	/**
	 * Checks if the audio format if the chunk is equal to the specified
	 * audio_format.
	 */
	gcc_pure
	bool CheckFormat(AudioFormat other) const noexcept {
		return !audio_format.IsDefined() ||
			audio_format == other;
	}

	/**
	 * Checks if the specified chunk is enqueued in the music pipe.
	 */
	gcc_pure
	bool Contains(const MusicChunk *chunk) const noexcept;
#endif

	/**
	 * Returns the first #MusicChunk from the pipe.  Returns
	 * nullptr if the pipe is empty.
	 */
	gcc_pure
	const MusicChunk *Peek() const noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		return head;
	}

	/**
	 * Removes the first chunk from the head, and returns it.
	 */
	MusicChunk *Shift() noexcept;

	/**
	 * Clears the whole pipe and returns the chunks to the buffer.
	 *
	 * @param buffer the buffer object to return the chunks to
	 */
	void Clear(MusicBuffer &buffer) noexcept;

	/**
	 * Pushes a chunk to the tail of the pipe.
	 */
	void Push(MusicChunk *chunk) noexcept;

	/**
	 * Returns the number of chunks currently in this pipe.
	 */
	gcc_pure
	unsigned GetSize() const noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		return size;
	}

	gcc_pure
	bool IsEmpty() const noexcept {
		return GetSize() == 0;
	}
};

#endif

/*
 * Copyright (C) 2013 Max Kellermann <max@duempel.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef MPD_MANUAL_HXX
#define MPD_MANUAL_HXX

#include "Compiler.h"

#include <new>
#include <utility>

#include <assert.h>

#if CLANG_OR_GCC_VERSION(4,7)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

/**
 * Container for an object that gets constructed and destructed
 * manually.  The object is constructed in-place, and therefore
 * without allocation overhead.  It can be constructed and destructed
 * repeatedly.
 */
template<class T>
class Manual {
	alignas(T)
	char data[sizeof(T)];

#ifndef NDEBUG
	bool initialized;
#endif

public:
#ifndef NDEBUG
	Manual():initialized(false) {}
	~Manual() {
		assert(!initialized);
	}
#endif

	template<typename... Args>
	void Construct(Args&&... args) {
		assert(!initialized);

		void *p = data;
		new(p) T(std::forward<Args>(args)...);

#ifndef NDEBUG
		initialized = true;
#endif
	}

	void Destruct() {
		assert(initialized);

		T &t = Get();
		t.T::~T();

#ifndef NDEBUG
		initialized = false;
#endif
	}

	T &Get() {
		assert(initialized);

		void *p = static_cast<void *>(data);
		return *static_cast<T *>(p);
	}

	const T &Get() const {
		assert(initialized);

		const void *p = static_cast<const void *>(data);
		return *static_cast<const T *>(p);
	}

	operator T &() {
		return Get();
	}

	operator const T &() const {
		return Get();
	}

	T *operator->() {
		return &Get();
	}

	const T *operator->() const {
		return &Get();
	}
};

#if CLANG_OR_GCC_VERSION(4,7)
#pragma GCC diagnostic pop
#endif

#endif

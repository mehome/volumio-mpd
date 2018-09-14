/*
 * Copyright (C) 2008-2016 Max Kellermann <max.kellermann@gmail.com>
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

#include "config.h"
#include "Global.hxx"
#include "Request.hxx"
#include "IOThread.hxx"
#include "Log.hxx"
#include "event/SocketMonitor.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"

static constexpr Domain curlm_domain("curlm");

/**
 * Monitor for one socket created by CURL.
 */
class CurlSocket final : SocketMonitor {
	CurlGlobal &global;

public:
	CurlSocket(CurlGlobal &_global, EventLoop &_loop, int _fd)
		:SocketMonitor(_fd, _loop), global(_global) {}

	~CurlSocket() {
		/* TODO: sometimes, CURL uses CURL_POLL_REMOVE after
		   closing the socket, and sometimes, it uses
		   CURL_POLL_REMOVE just to move the (still open)
		   connection to the pool; in the first case,
		   Abandon() would be most appropriate, but it breaks
		   the second case - is that a CURL bug?  is there a
		   better solution? */
	}

	/**
	 * Callback function for CURLMOPT_SOCKETFUNCTION.
	 */
	static int SocketFunction(CURL *easy,
				  curl_socket_t s, int action,
				  void *userp, void *socketp) noexcept;

	virtual bool OnSocketReady(unsigned flags) override;

private:
	static constexpr int FlagsToCurlCSelect(unsigned flags) {
		return (flags & (READ | HANGUP) ? CURL_CSELECT_IN : 0) |
			(flags & WRITE ? CURL_CSELECT_OUT : 0) |
			(flags & ERROR ? CURL_CSELECT_ERR : 0);
	}

	gcc_const
	static unsigned CurlPollToFlags(int action) noexcept {
		switch (action) {
		case CURL_POLL_NONE:
			return 0;

		case CURL_POLL_IN:
			return READ;

		case CURL_POLL_OUT:
			return WRITE;

		case CURL_POLL_INOUT:
			return READ|WRITE;
		}

		assert(false);
		gcc_unreachable();
	}
};

CurlGlobal::CurlGlobal(EventLoop &_loop)
	:TimeoutMonitor(_loop), DeferredMonitor(_loop)
{
	multi.SetOption(CURLMOPT_SOCKETFUNCTION, CurlSocket::SocketFunction);
	multi.SetOption(CURLMOPT_SOCKETDATA, this);

	multi.SetOption(CURLMOPT_TIMERFUNCTION, TimerFunction);
	multi.SetOption(CURLMOPT_TIMERDATA, this);
}

int
CurlSocket::SocketFunction(gcc_unused CURL *easy,
			   curl_socket_t s, int action,
			   void *userp, void *socketp) noexcept {
	auto &global = *(CurlGlobal *)userp;
	CurlSocket *cs = (CurlSocket *)socketp;

	assert(io_thread_inside());

	if (action == CURL_POLL_REMOVE) {
		delete cs;
		return 0;
	}

	if (cs == nullptr) {
		cs = new CurlSocket(global, io_thread_get(), s);
		global.Assign(s, *cs);
	} else {
#ifdef USE_EPOLL
		/* when using epoll, we need to unregister the socket
		   each time this callback is invoked, because older
		   CURL versions may omit the CURL_POLL_REMOVE call
		   when the socket has been closed and recreated with
		   the same file number (bug found in CURL 7.26, CURL
		   7.33 not affected); in that case, epoll refuses the
		   EPOLL_CTL_MOD because it does not know the new
		   socket yet */
		cs->Cancel();
#endif
	}

	unsigned flags = CurlPollToFlags(action);
	if (flags != 0)
		cs->Schedule(flags);
	return 0;
}

bool
CurlSocket::OnSocketReady(unsigned flags)
{
	assert(io_thread_inside());

	global.SocketAction(Get(), FlagsToCurlCSelect(flags));
	return true;
}

/**
 * Runs in the I/O thread.  No lock needed.
 *
 * Throws std::runtime_error on error.
 */
void
CurlGlobal::Add(CURL *easy, CurlRequest &request)
{
	assert(io_thread_inside());
	assert(easy != nullptr);

	curl_easy_setopt(easy, CURLOPT_PRIVATE, &request);

	CURLMcode mcode = curl_multi_add_handle(multi.Get(), easy);
	if (mcode != CURLM_OK)
		throw FormatRuntimeError("curl_multi_add_handle() failed: %s",
					 curl_multi_strerror(mcode));

	InvalidateSockets();
}

void
CurlGlobal::Remove(CURL *easy)
{
	assert(io_thread_inside());
	assert(easy != nullptr);

	curl_multi_remove_handle(multi.Get(), easy);

	InvalidateSockets();
}

static CurlRequest *
ToRequest(CURL *easy)
{
	void *p;
	CURLcode code = curl_easy_getinfo(easy, CURLINFO_PRIVATE, &p);
	if (code != CURLE_OK)
		return nullptr;

	return (CurlRequest *)p;
}

/**
 * Check for finished HTTP responses.
 *
 * Runs in the I/O thread.  The caller must not hold locks.
 */
inline void
CurlGlobal::ReadInfo()
{
	assert(io_thread_inside());

	CURLMsg *msg;
	int msgs_in_queue;

	while ((msg = curl_multi_info_read(multi.Get(),
					   &msgs_in_queue)) != nullptr) {
		if (msg->msg == CURLMSG_DONE) {
			auto *request = ToRequest(msg->easy_handle);
			if (request != nullptr)
				request->Done(msg->data.result);
		}
	}
}

inline void
CurlGlobal::UpdateTimeout(long timeout_ms)
{
	if (timeout_ms < 0) {
		TimeoutMonitor::Cancel();
		return;
	}

	if (timeout_ms < 10)
		/* CURL 7.21.1 likes to report "timeout=0", which
		   means we're running in a busy loop.  Quite a bad
		   idea to waste so much CPU.  Let's use a lower limit
		   of 10ms. */
		timeout_ms = 10;

	TimeoutMonitor::Schedule(std::chrono::milliseconds(timeout_ms));
}

int
CurlGlobal::TimerFunction(gcc_unused CURLM *_global, long timeout_ms, void *userp)
{
	auto &global = *(CurlGlobal *)userp;
	assert(_global == global.multi.Get());

	global.UpdateTimeout(timeout_ms);
	return 0;
}

void
CurlGlobal::OnTimeout()
{
	SocketAction(CURL_SOCKET_TIMEOUT, 0);
}

void
CurlGlobal::SocketAction(curl_socket_t fd, int ev_bitmask)
{
	int running_handles;
	CURLMcode mcode = curl_multi_socket_action(multi.Get(), fd, ev_bitmask,
						   &running_handles);
	if (mcode != CURLM_OK)
		FormatError(curlm_domain,
			    "curl_multi_socket_action() failed: %s",
			    curl_multi_strerror(mcode));

	DeferredMonitor::Schedule();
}

void
CurlGlobal::RunDeferred()
{
	ReadInfo();
}

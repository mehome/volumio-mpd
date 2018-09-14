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
#include "Loop.hxx"
#include "TimeoutMonitor.hxx"
#include "SocketMonitor.hxx"
#include "IdleMonitor.hxx"
#include "DeferredMonitor.hxx"

#include <algorithm>

EventLoop::EventLoop()
	:SocketMonitor(*this), quit(false)
{
	SocketMonitor::Open(wake_fd.Get());
	SocketMonitor::Schedule(SocketMonitor::READ);
}

EventLoop::~EventLoop()
{
	assert(idle.empty());
	assert(timers.empty());

	/* this is necessary to get a well-defined destruction
	   order */
	SocketMonitor::Cancel();
}

void
EventLoop::Break()
{
	if (quit.exchange(true))
		return;

	wake_fd.Write();
}

bool
EventLoop::Abandon(int _fd, SocketMonitor &m)
{
	assert(IsInsideOrVirgin());

	poll_result.Clear(&m);
	return poll_group.Abandon(_fd);
}

bool
EventLoop::RemoveFD(int _fd, SocketMonitor &m)
{
	assert(IsInsideOrNull());

	poll_result.Clear(&m);
	return poll_group.Remove(_fd);
}

void
EventLoop::AddIdle(IdleMonitor &i)
{
	assert(IsInsideOrVirgin());
	assert(std::find(idle.begin(), idle.end(), &i) == idle.end());

	idle.push_back(&i);
	again = true;
}

void
EventLoop::RemoveIdle(IdleMonitor &i)
{
	assert(IsInsideOrVirgin());

	auto it = std::find(idle.begin(), idle.end(), &i);
	assert(it != idle.end());

	idle.erase(it);
}

void
EventLoop::AddTimer(TimeoutMonitor &t, std::chrono::steady_clock::duration d)
{
	/* can't use IsInsideOrVirgin() here because libavahi-client
	   modifies the timeout during avahi_client_free() */
	assert(IsInsideOrNull());

	timers.insert(TimerRecord(t, now + d));
	again = true;
}

void
EventLoop::CancelTimer(TimeoutMonitor &t)
{
	assert(IsInsideOrNull());

	for (auto i = timers.begin(), end = timers.end(); i != end; ++i) {
		if (&i->timer == &t) {
			timers.erase(i);
			return;
		}
	}
}

/**
 * Convert the given timeout specification to a milliseconds integer,
 * to be used by functions like poll() and epoll_wait().  Any negative
 * value (= never times out) is translated to the magic value -1.
 */
static constexpr int
ExportTimeoutMS(std::chrono::steady_clock::duration timeout)
{
	return timeout >= timeout.zero()
		? int(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count())
		: -1;
}

void
EventLoop::Run()
{
	assert(thread.IsNull());
	assert(virgin);

#ifndef NDEBUG
	virgin = false;
#endif

	thread = ThreadId::GetCurrent();

	assert(!quit);
	assert(busy);

	do {
		now = std::chrono::steady_clock::now();
		again = false;

		/* invoke timers */

		std::chrono::steady_clock::duration timeout;
		while (true) {
			auto i = timers.begin();
			if (i == timers.end()) {
				timeout = std::chrono::steady_clock::duration(-1);
				break;
			}

			timeout = i->due - now;
			if (timeout > timeout.zero())
				break;

			TimeoutMonitor &m = i->timer;
			timers.erase(i);

			m.Run();

			if (quit)
				return;
		}

		/* invoke idle */

		while (!idle.empty()) {
			IdleMonitor &m = *idle.front();
			idle.pop_front();
			m.Run();

			if (quit)
				return;
		}

		/* try to handle DeferredMonitors without WakeFD
		   overhead */
		mutex.lock();
		HandleDeferred();
		busy = false;
		const bool _again = again;
		mutex.unlock();

		if (_again)
			/* re-evaluate timers because one of the
			   IdleMonitors may have added a new
			   timeout */
			continue;

		/* wait for new event */

		poll_group.ReadEvents(poll_result, ExportTimeoutMS(timeout));

		now = std::chrono::steady_clock::now();

		mutex.lock();
		busy = true;
		mutex.unlock();

		/* invoke sockets */
		for (int i = 0; i < poll_result.GetSize(); ++i) {
			auto events = poll_result.GetEvents(i);
			if (events != 0) {
				if (quit)
					break;

				auto m = (SocketMonitor *)poll_result.GetObject(i);
				m->Dispatch(events);
			}
		}

		poll_result.Reset();

	} while (!quit);

#ifndef NDEBUG
	assert(busy);
	assert(thread.IsInside());
#endif

	thread = ThreadId::Null();
}

void
EventLoop::AddDeferred(DeferredMonitor &d)
{
	mutex.lock();
	if (d.pending) {
		mutex.unlock();
		return;
	}

	assert(std::find(deferred.begin(),
			 deferred.end(), &d) == deferred.end());

	/* we don't need to wake up the EventLoop if another
	   DeferredMonitor has already done it */
	const bool must_wake = !busy && deferred.empty();

	d.pending = true;
	deferred.push_back(&d);
	again = true;
	mutex.unlock();

	if (must_wake)
		wake_fd.Write();
}

void
EventLoop::RemoveDeferred(DeferredMonitor &d)
{
	const std::lock_guard<Mutex> protect(mutex);

	if (!d.pending) {
		assert(std::find(deferred.begin(),
				 deferred.end(), &d) == deferred.end());
		return;
	}

	d.pending = false;

	auto i = std::find(deferred.begin(), deferred.end(), &d);
	assert(i != deferred.end());

	deferred.erase(i);
}

void
EventLoop::HandleDeferred()
{
	while (!deferred.empty() && !quit) {
		DeferredMonitor &m = *deferred.front();
		assert(m.pending);

		deferred.pop_front();
		m.pending = false;

		mutex.unlock();
		m.RunDeferred();
		mutex.lock();
	}
}

bool
EventLoop::OnSocketReady(gcc_unused unsigned flags)
{
	assert(IsInside());

	wake_fd.Read();

	mutex.lock();
	HandleDeferred();
	mutex.unlock();

	return true;
}

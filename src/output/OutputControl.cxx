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
#include "Internal.hxx"
#include "OutputPlugin.hxx"
#include "Domain.hxx"
#include "mixer/MixerControl.hxx"
#include "notify.hxx"
#include "filter/plugins/ReplayGainFilterPlugin.hxx"
#include "Log.hxx"

#include <stdexcept>

#include <assert.h>

/** after a failure, wait this duration before
    automatically reopening the device */
static constexpr PeriodClock::Duration REOPEN_AFTER = std::chrono::seconds(10);

struct notify audio_output_client_notify;

void
AudioOutput::WaitForCommand()
{
	while (!IsCommandFinished()) {
		mutex.unlock();
		audio_output_client_notify.Wait();
		mutex.lock();
	}
}

void
AudioOutput::CommandAsync(Command cmd)
{
	assert(IsCommandFinished());

	command = cmd;
	cond.signal();
}

void
AudioOutput::CommandWait(Command cmd)
{
	CommandAsync(cmd);
	WaitForCommand();
}

void
AudioOutput::LockCommandWait(Command cmd)
{
	const std::lock_guard<Mutex> protect(mutex);
	CommandWait(cmd);
}

void
AudioOutput::EnableAsync()
{
	if (!thread.IsDefined()) {
		if (plugin.enable == nullptr) {
			/* don't bother to start the thread now if the
			   device doesn't even have a enable() method;
			   just assign the variable and we're done */
			really_enabled = true;
			return;
		}

		StartThread();
	}

	CommandAsync(Command::ENABLE);
}

void
AudioOutput::DisableAsync()
{
	if (!thread.IsDefined()) {
		if (plugin.disable == nullptr)
			really_enabled = false;
		else
			/* if there's no thread yet, the device cannot
			   be enabled */
			assert(!really_enabled);

		return;
	}

	CommandAsync(Command::DISABLE);
}

inline bool
AudioOutput::Open(const AudioFormat audio_format, const MusicPipe &mp)
{
	assert(allow_play);
	assert(audio_format.IsValid());

	fail_timer.Reset();

	if (open && audio_format == request.audio_format) {
		assert(request.pipe == &mp || (always_on && pause));

		if (!pause)
			/* already open, already the right parameters
			   - nothing needs to be done */
			return true;
	}

	request.audio_format = audio_format;
	request.pipe = &mp;

	if (!thread.IsDefined())
		StartThread();

	CommandWait(Command::OPEN);
	const bool open2 = open;

	if (open2 && mixer != nullptr) {
		try {
			mixer_open(mixer);
		} catch (const std::runtime_error &e) {
			FormatError(e, "Failed to open mixer for '%s'", name);
		}
	}

	return open2;
}

void
AudioOutput::CloseWait()
{
	assert(allow_play);

	if (mixer != nullptr)
		mixer_auto_close(mixer);

	assert(!open || !fail_timer.IsDefined());

	if (open)
		CommandWait(Command::CLOSE);
	else
		fail_timer.Reset();
}

bool
AudioOutput::LockUpdate(const AudioFormat audio_format,
			const MusicPipe &mp,
			bool force)
{
	const std::lock_guard<Mutex> protect(mutex);

	if (enabled && really_enabled) {
		if (force || !fail_timer.IsDefined() ||
		    fail_timer.Check(REOPEN_AFTER * 1000)) {
			return Open(audio_format, mp);
		}
	} else if (IsOpen())
		CloseWait();

	return false;
}

void
AudioOutput::LockPlay()
{
	const std::lock_guard<Mutex> protect(mutex);

	assert(allow_play);

	if (IsOpen() && !in_playback_loop && !woken_for_play) {
		woken_for_play = true;
		cond.signal();
	}
}

void
AudioOutput::LockPauseAsync()
{
	if (mixer != nullptr && plugin.pause == nullptr)
		/* the device has no pause mode: close the mixer,
		   unless its "global" flag is set (checked by
		   mixer_auto_close()) */
		mixer_auto_close(mixer);

	const std::lock_guard<Mutex> protect(mutex);

	assert(allow_play);
	if (IsOpen())
		CommandAsync(Command::PAUSE);
}

void
AudioOutput::LockDrainAsync()
{
	const std::lock_guard<Mutex> protect(mutex);

	assert(allow_play);
	if (IsOpen())
		CommandAsync(Command::DRAIN);
}

void
AudioOutput::LockCancelAsync()
{
	const std::lock_guard<Mutex> protect(mutex);

	if (IsOpen()) {
		allow_play = false;
		CommandAsync(Command::CANCEL);
	}
}

void
AudioOutput::LockAllowPlay()
{
	const std::lock_guard<Mutex> protect(mutex);

	allow_play = true;
	if (IsOpen())
		cond.signal();
}

void
AudioOutput::LockRelease()
{
	if (always_on)
		LockPauseAsync();
	else
		LockCloseWait();
}

void
AudioOutput::LockCloseWait()
{
	assert(!open || !fail_timer.IsDefined());

	const std::lock_guard<Mutex> protect(mutex);
	CloseWait();
}

void
AudioOutput::StopThread()
{
	assert(thread.IsDefined());
	assert(allow_play);

	LockCommandWait(Command::KILL);
	thread.Join();
}

void
AudioOutput::BeginDestroy()
{
	if (mixer != nullptr)
		mixer_auto_close(mixer);

	if (thread.IsDefined()) {
		const std::lock_guard<Mutex> protect(mutex);
		CommandAsync(Command::KILL);
	}
}

void
AudioOutput::FinishDestroy()
{
	if (thread.IsDefined())
		thread.Join();

	audio_output_free(this);
}

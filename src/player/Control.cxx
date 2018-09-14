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
#include "Control.hxx"
#include "Idle.hxx"
#include "DetachedSong.hxx"
#include "output/MultipleOutputs.hxx"

#include <algorithm>

#include <assert.h>

PlayerControl::PlayerControl(PlayerListener &_listener,
			     MultipleOutputs &_outputs,
			     unsigned _buffer_chunks,
			     unsigned _buffered_before_play,
			     AudioFormat _configured_audio_format,
			     const ReplayGainConfig &_replay_gain_config)
	:listener(_listener), outputs(_outputs),
	 buffer_chunks(_buffer_chunks),
	 buffered_before_play(_buffered_before_play),
	 configured_audio_format(_configured_audio_format),
	 thread(BIND_THIS_METHOD(RunThread)),
	 replay_gain_config(_replay_gain_config)
{
}

PlayerControl::~PlayerControl()
{
	delete next_song;
	delete tagged_song;
}

bool
PlayerControl::WaitOutputConsumed(unsigned threshold)
{
	bool result = outputs.Check() < threshold;
	if (!result && command == PlayerCommand::NONE) {
		Wait();
		result = outputs.Check() < threshold;
	}

	return result;
}

void
PlayerControl::Play(DetachedSong *song)
{
	assert(song != nullptr);

	const std::lock_guard<Mutex> protect(mutex);
	SeekLocked(song, SongTime::zero());

	if (state == PlayerState::PAUSE)
		/* if the player was paused previously, we need to
		   unpause it */
		PauseLocked();
}

void
PlayerControl::LockCancel()
{
	LockSynchronousCommand(PlayerCommand::CANCEL);
	assert(next_song == nullptr);
}

void
PlayerControl::LockStop()
{
	LockSynchronousCommand(PlayerCommand::CLOSE_AUDIO);
	assert(next_song == nullptr);

	idle_add(IDLE_PLAYER);
}

void
PlayerControl::LockUpdateAudio()
{
	LockSynchronousCommand(PlayerCommand::UPDATE_AUDIO);
}

void
PlayerControl::Kill()
{
	assert(thread.IsDefined());

	LockSynchronousCommand(PlayerCommand::EXIT);
	thread.Join();

	idle_add(IDLE_PLAYER);
}

void
PlayerControl::PauseLocked()
{
	if (state != PlayerState::STOP) {
		SynchronousCommand(PlayerCommand::PAUSE);
		idle_add(IDLE_PLAYER);
	}
}

void
PlayerControl::LockPause()
{
	const std::lock_guard<Mutex> protect(mutex);
	PauseLocked();
}

void
PlayerControl::LockSetPause(bool pause_flag)
{
	const std::lock_guard<Mutex> protect(mutex);

	switch (state) {
	case PlayerState::STOP:
		break;

	case PlayerState::PLAY:
		if (pause_flag)
			PauseLocked();
		break;

	case PlayerState::PAUSE:
		if (!pause_flag)
			PauseLocked();
		break;
	}
}

void
PlayerControl::LockSetBorderPause(bool _border_pause)
{
	const std::lock_guard<Mutex> protect(mutex);
	border_pause = _border_pause;
}

player_status
PlayerControl::LockGetStatus() noexcept
{
	player_status status;

	const std::lock_guard<Mutex> protect(mutex);
	SynchronousCommand(PlayerCommand::REFRESH);

	status.state = state;

	if (state != PlayerState::STOP) {
		status.bit_rate = bit_rate;
		status.audio_format = audio_format;
		status.total_time = total_time;
		status.elapsed_time = elapsed_time;
	}

	return status;
}

void
PlayerControl::SetError(PlayerError type, std::exception_ptr &&_error)
{
	assert(type != PlayerError::NONE);
	assert(_error);

	error_type = type;
	error = std::move(_error);
}

void
PlayerControl::LockClearError()
{
	const std::lock_guard<Mutex> protect(mutex);
	ClearError();
}

void
PlayerControl::LockSetTaggedSong(const DetachedSong &song)
{
	const std::lock_guard<Mutex> protect(mutex);
	delete tagged_song;
	tagged_song = new DetachedSong(song);
}

void
PlayerControl::ClearTaggedSong()
{
	delete tagged_song;
	tagged_song = nullptr;
}

void
PlayerControl::LockEnqueueSong(DetachedSong *song)
{
	assert(song != nullptr);

	const std::lock_guard<Mutex> protect(mutex);
	EnqueueSongLocked(song);
}

void
PlayerControl::SeekLocked(DetachedSong *song, SongTime t)
{
	assert(song != nullptr);

	/* to issue the SEEK command below, we need to clear the
	   "next_song" attribute with the CANCEL command */
	/* optimization TODO: if the decoder happens to decode that
	   song already, don't cancel that */
	if (next_song != nullptr)
		SynchronousCommand(PlayerCommand::CANCEL);

	assert(next_song == nullptr);

	ClearError();
	next_song = song;
	seek_time = t;
	SynchronousCommand(PlayerCommand::SEEK);

	assert(next_song == nullptr);

	if (error_type != PlayerError::NONE) {
		assert(error);
		std::rethrow_exception(error);
	}

	assert(!error);
}

void
PlayerControl::LockSeek(DetachedSong *song, SongTime t)
{
	assert(song != nullptr);

	{
		const std::lock_guard<Mutex> protect(mutex);
		SeekLocked(song, t);
	}

	idle_add(IDLE_PLAYER);
}

void
PlayerControl::SetCrossFade(float _cross_fade_seconds)
{
	if (_cross_fade_seconds < 0)
		_cross_fade_seconds = 0;
	cross_fade.duration = _cross_fade_seconds;

	idle_add(IDLE_OPTIONS);
}

void
PlayerControl::SetMixRampDb(float _mixramp_db)
{
	cross_fade.mixramp_db = _mixramp_db;

	idle_add(IDLE_OPTIONS);
}

void
PlayerControl::SetMixRampDelay(float _mixramp_delay_seconds)
{
	cross_fade.mixramp_delay = _mixramp_delay_seconds;

	idle_add(IDLE_OPTIONS);
}

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
#include "GmeDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "config/Block.cxx"
#include "CheckAudioFormat.hxx"
#include "DetachedSong.hxx"
#include "tag/TagHandler.hxx"
#include "tag/TagBuilder.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringFormat.hxx"
#include "util/UriUtil.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <gme/gme.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SUBTUNE_PREFIX "tune_"

static constexpr Domain gme_domain("gme");

static constexpr unsigned GME_SAMPLE_RATE = 44100;
static constexpr unsigned GME_CHANNELS = 2;
static constexpr unsigned GME_BUFFER_FRAMES = 2048;
static constexpr unsigned GME_BUFFER_SAMPLES =
	GME_BUFFER_FRAMES * GME_CHANNELS;

struct GmeContainerPath {
	AllocatedPath path;
	unsigned track;
};

#if GME_VERSION >= 0x000600
static int gme_accuracy;
#endif

static bool
gme_plugin_init(gcc_unused const ConfigBlock &block)
{
#if GME_VERSION >= 0x000600
	auto accuracy = block.GetBlockParam("accuracy");
	gme_accuracy = accuracy != nullptr
		? (int)accuracy->GetBoolValue()
		: -1;
#endif

	return true;
}

gcc_pure
static unsigned
ParseSubtuneName(const char *base) noexcept
{
	if (memcmp(base, SUBTUNE_PREFIX, sizeof(SUBTUNE_PREFIX) - 1) != 0)
		return 0;

	base += sizeof(SUBTUNE_PREFIX) - 1;

	char *endptr;
	auto track = strtoul(base, &endptr, 10);
	if (endptr == base || *endptr != '.')
		return 0;

	return track;
}

/**
 * returns the file path stripped of any /tune_xxx.* subtune suffix
 * and the track number (or 0 if no "tune_xxx" suffix is present).
 */
static GmeContainerPath
ParseContainerPath(Path path_fs)
{
	const Path base = path_fs.GetBase();
	unsigned track;
	if (base.IsNull() ||
	    (track = ParseSubtuneName(base.c_str())) < 1)
		return { AllocatedPath(path_fs), 0 };

	return { path_fs.GetDirectoryName(), track - 1 };
}

static void
gme_file_decode(DecoderClient &client, Path path_fs)
{
	const auto container = ParseContainerPath(path_fs);

	Music_Emu *emu;
	const char *gme_err =
		gme_open_file(container.path.c_str(), &emu, GME_SAMPLE_RATE);
	if (gme_err != nullptr) {
		LogWarning(gme_domain, gme_err);
		return;
	}

	AtScopeExit(emu) { gme_delete(emu); };

	FormatDebug(gme_domain, "emulator type '%s'\n",
		    gme_type_system(gme_type(emu)));

#if GME_VERSION >= 0x000600
	if (gme_accuracy >= 0)
		gme_enable_accuracy(emu, gme_accuracy);
#endif

	gme_info_t *ti;
	gme_err = gme_track_info(emu, &ti, container.track);
	if (gme_err != nullptr) {
		LogWarning(gme_domain, gme_err);
		return;
	}

	const int length = ti->play_length;
	gme_free_info(ti);

	const SignedSongTime song_len = length > 0
		? SignedSongTime::FromMS(length)
		: SignedSongTime::Negative();

	/* initialize the MPD decoder */

	const auto audio_format = CheckAudioFormat(GME_SAMPLE_RATE,
						   SampleFormat::S16,
						   GME_CHANNELS);

	client.Ready(audio_format, true, song_len);

	gme_err = gme_start_track(emu, container.track);
	if (gme_err != nullptr)
		LogWarning(gme_domain, gme_err);

	if (length > 0)
		gme_set_fade(emu, length);

	/* play */
	DecoderCommand cmd;
	do {
		short buf[GME_BUFFER_SAMPLES];
		gme_err = gme_play(emu, GME_BUFFER_SAMPLES, buf);
		if (gme_err != nullptr) {
			LogWarning(gme_domain, gme_err);
			return;
		}

		cmd = client.SubmitData(nullptr, buf, sizeof(buf), 0);
		if (cmd == DecoderCommand::SEEK) {
			unsigned where = client.GetSeekTime().ToMS();
			gme_err = gme_seek(emu, where);
			if (gme_err != nullptr) {
				LogWarning(gme_domain, gme_err);
				client.SeekError();
			} else
				client.CommandFinished();
		}

		if (gme_track_ended(emu))
			break;
	} while (cmd != DecoderCommand::STOP);
}

static void
ScanGmeInfo(const gme_info_t &info, unsigned song_num, int track_count,
	    const TagHandler &handler, void *handler_ctx)
{
	if (info.play_length > 0)
		tag_handler_invoke_duration(handler, handler_ctx,
					    SongTime::FromMS(info.play_length));

	if (track_count > 1)
		tag_handler_invoke_tag(handler, handler_ctx, TAG_TRACK,
				       StringFormat<16>("%u", song_num + 1));

	if (info.song != nullptr) {
		if (track_count > 1) {
			/* start numbering subtunes from 1 */
			const auto tag_title =
				StringFormat<1024>("%s (%u/%d)",
						   info.song, song_num + 1,
						   track_count);
			tag_handler_invoke_tag(handler, handler_ctx,
					       TAG_TITLE, tag_title);
		} else
			tag_handler_invoke_tag(handler, handler_ctx,
					       TAG_TITLE, info.song);
	}

	if (info.author != nullptr)
		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_ARTIST, info.author);

	if (info.game != nullptr)
		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_ALBUM, info.game);

	if (info.comment != nullptr)
		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_COMMENT, info.comment);

	if (info.copyright != nullptr)
		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_DATE, info.copyright);
}

static bool
ScanMusicEmu(Music_Emu *emu, unsigned song_num,
	     const TagHandler &handler, void *handler_ctx)
{
	gme_info_t *ti;
	const char *gme_err = gme_track_info(emu, &ti, song_num);
	if (gme_err != nullptr) {
		LogWarning(gme_domain, gme_err);
		return false;
	}

	assert(ti != nullptr);

	AtScopeExit(ti) { gme_free_info(ti); };

	ScanGmeInfo(*ti, song_num, gme_track_count(emu),
		    handler, handler_ctx);
	return true;
}

static bool
gme_scan_file(Path path_fs,
	      const TagHandler &handler, void *handler_ctx)
{
	const auto container = ParseContainerPath(path_fs);

	Music_Emu *emu;
	const char *gme_err =
		gme_open_file(container.path.c_str(), &emu, GME_SAMPLE_RATE);
	if (gme_err != nullptr) {
		LogWarning(gme_domain, gme_err);
		return false;
	}

	AtScopeExit(emu) { gme_delete(emu); };

	return ScanMusicEmu(emu, container.track, handler, handler_ctx);
}

static std::forward_list<DetachedSong>
gme_container_scan(Path path_fs)
{
	std::forward_list<DetachedSong> list;

	Music_Emu *emu;
	const char *gme_err = gme_open_file(path_fs.c_str(), &emu,
					    GME_SAMPLE_RATE);
	if (gme_err != nullptr) {
		LogWarning(gme_domain, gme_err);
		return list;
	}

	AtScopeExit(emu) { gme_delete(emu); };

	const unsigned num_songs = gme_track_count(emu);
	/* if it only contains a single tune, don't treat as container */
	if (num_songs < 2)
		return list;

	const char *subtune_suffix = uri_get_suffix(path_fs.c_str());

	TagBuilder tag_builder;

	auto tail = list.before_begin();
	for (unsigned i = 0; i < num_songs; ++i) {
		ScanMusicEmu(emu, i,
			     add_tag_handler, &tag_builder);

		const auto track_name =
			StringFormat<64>(SUBTUNE_PREFIX "%03u.%s", i+1,
					 subtune_suffix);
		tail = list.emplace_after(tail, track_name,
					  tag_builder.Commit());
	}

	return list;
}

static const char *const gme_suffixes[] = {
	"ay", "gbs", "gym", "hes", "kss", "nsf",
	"nsfe", "sap", "spc", "vgm", "vgz",
	nullptr
};

const struct DecoderPlugin gme_decoder_plugin = {
	"gme",
	gme_plugin_init,
	nullptr,
	nullptr,
	gme_file_decode,
	gme_scan_file,
	nullptr,
	gme_container_scan,
	gme_suffixes,
	nullptr,
};

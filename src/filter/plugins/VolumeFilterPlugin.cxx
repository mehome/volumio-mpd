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
#include "VolumeFilterPlugin.hxx"
#include "filter/FilterPlugin.hxx"
#include "filter/FilterInternal.hxx"
#include "filter/FilterRegistry.hxx"
#include "pcm/Volume.hxx"
#include "AudioFormat.hxx"
#include "util/ConstBuffer.hxx"

#include <stdexcept>

class VolumeFilter final : public Filter {
	PcmVolume pv;

public:
	explicit VolumeFilter(const AudioFormat &audio_format)
		:Filter(audio_format) {
		pv.Open(out_audio_format.format);
	}

	unsigned GetVolume() const {
		return pv.GetVolume();
	}

	void SetVolume(unsigned _volume) {
		pv.SetVolume(_volume);
	}

	/* virtual methods from class Filter */
	ConstBuffer<void> FilterPCM(ConstBuffer<void> src) override;
};

class PreparedVolumeFilter final : public PreparedFilter {
	PcmVolume pv;

public:
	/* virtual methods from class Filter */
	Filter *Open(AudioFormat &af) override;
};

static PreparedFilter *
volume_filter_init(gcc_unused const ConfigBlock &block)
{
	return new PreparedVolumeFilter();
}

Filter *
PreparedVolumeFilter::Open(AudioFormat &audio_format)
{
	return new VolumeFilter(audio_format);
}

ConstBuffer<void>
VolumeFilter::FilterPCM(ConstBuffer<void> src)
{
	return pv.Apply(src);
}

const FilterPlugin volume_filter_plugin = {
	"volume",
	volume_filter_init,
};

unsigned
volume_filter_get(const Filter *_filter)
{
	const VolumeFilter *filter =
		(const VolumeFilter *)_filter;

	return filter->GetVolume();
}

void
volume_filter_set(Filter *_filter, unsigned volume)
{
	VolumeFilter *filter = (VolumeFilter *)_filter;

	filter->SetVolume(volume);
}


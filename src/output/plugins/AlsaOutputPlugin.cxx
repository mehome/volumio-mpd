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
#include "AlsaOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "../Wrapper.hxx"
#include "mixer/MixerList.hxx"
#include "pcm/PcmExport.hxx"
#include "system/ByteOrder.hxx"
#include "util/Manual.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "util/ConstBuffer.hxx"
#include "Log.hxx"

#include <alsa/asoundlib.h>

#include <string>

#if SND_LIB_VERSION >= 0x1001c
/* alsa-lib supports DSD since version 1.0.27.1 */
#define HAVE_ALSA_DSD
#endif

#if SND_LIB_VERSION >= 0x1001d
/* alsa-lib supports DSD_U32 since version 1.0.29 */
#define HAVE_ALSA_DSD_U32
#endif

static const char default_device[] = "default";

static constexpr unsigned MPD_ALSA_BUFFER_TIME_US = 500000;

static constexpr unsigned MPD_ALSA_RETRY_NR = 5;

class AlsaOutput {
	friend struct AudioOutputWrapper<AlsaOutput>;

	AudioOutput base;

	Manual<PcmExport> pcm_export;

	/**
	 * The configured name of the ALSA device; empty for the
	 * default device
	 */
	const std::string device;

#ifdef ENABLE_DSD
	/**
	 * Enable DSD over PCM according to the DoP standard?
	 *
	 * @see http://dsd-guide.com/dop-open-standard
	 */
	const bool dop;
#endif

	/** libasound's buffer_time setting (in microseconds) */
	const unsigned buffer_time;

	/** libasound's period_time setting (in microseconds) */
	const unsigned period_time;

	/** the mode flags passed to snd_pcm_open */
	int mode = 0;

	/** the libasound PCM device handle */
	snd_pcm_t *pcm;

	/**
	 * The size of one audio frame passed to method play().
	 */
	size_t in_frame_size;

	/**
	 * The size of one audio frame passed to libasound.
	 */
	size_t out_frame_size;

	/**
	 * The size of one period, in number of frames.
	 */
	snd_pcm_uframes_t period_frames;

	/**
	 * The number of frames written in the current period.
	 */
	snd_pcm_uframes_t period_position;

	/**
	 * Do we need to call snd_pcm_prepare() before the next write?
	 * It means that we put the device to SND_PCM_STATE_SETUP by
	 * calling snd_pcm_drop().
	 *
	 * Without this flag, we could easily recover after a failed
	 * optimistic write (returning -EBADFD), but the Raspberry Pi
	 * audio driver is infamous for generating ugly artefacts from
	 * this.
	 */
	bool must_prepare;

	/**
	 * This buffer gets allocated after opening the ALSA device.
	 * It contains silence samples, enough to fill one period (see
	 * #period_frames).
	 */
	uint8_t *silence;

public:
	AlsaOutput(const ConfigBlock &block);

	~AlsaOutput() {
		/* free libasound's config cache */
		snd_config_update_free_global();
	}

	gcc_pure
	const char *GetDevice() const noexcept {
		return device.empty() ? default_device : device.c_str();
	}

	static AlsaOutput *Create(const ConfigBlock &block);

	void Enable();
	void Disable();

	void Open(AudioFormat &audio_format);
	void Close();

	size_t PlayRaw(ConstBuffer<void> data);
	size_t Play(const void *chunk, size_t size);
	void Drain();
	void Cancel();

private:
	/**
	 * Set up the snd_pcm_t object which was opened by the caller.
	 * Set up the configured settings and the audio format.
	 *
	 * Throws #std::runtime_error on error.
	 */
	void Setup(AudioFormat &audio_format, PcmExport::Params &params);

#ifdef ENABLE_DSD
	void SetupDop(AudioFormat audio_format,
		      PcmExport::Params &params);
#endif

	void SetupOrDop(AudioFormat &audio_format, PcmExport::Params &params);

	int Recover(int err);

	/**
	 * Write silence to the ALSA device.
	 */
	void WriteSilence(snd_pcm_uframes_t nframes) {
		snd_pcm_writei(pcm, silence, nframes);
	}

};

static constexpr Domain alsa_output_domain("alsa_output");

AlsaOutput::AlsaOutput(const ConfigBlock &block)
	:base(alsa_output_plugin, block),
	 device(block.GetBlockValue("device", "")),
#ifdef ENABLE_DSD
	 dop(block.GetBlockValue("dop", false) ||
	     /* legacy name from MPD 0.18 and older: */
	     block.GetBlockValue("dsd_usb", false)),
#endif
	 buffer_time(block.GetBlockValue("buffer_time",
					 MPD_ALSA_BUFFER_TIME_US)),
	 period_time(block.GetBlockValue("period_time", 0u))
{
#ifdef SND_PCM_NO_AUTO_RESAMPLE
	if (!block.GetBlockValue("auto_resample", true))
		mode |= SND_PCM_NO_AUTO_RESAMPLE;
#endif

#ifdef SND_PCM_NO_AUTO_CHANNELS
	if (!block.GetBlockValue("auto_channels", true))
		mode |= SND_PCM_NO_AUTO_CHANNELS;
#endif

#ifdef SND_PCM_NO_AUTO_FORMAT
	if (!block.GetBlockValue("auto_format", true))
		mode |= SND_PCM_NO_AUTO_FORMAT;
#endif
}

inline AlsaOutput *
AlsaOutput::Create(const ConfigBlock &block)
{
	return new AlsaOutput(block);
}

inline void
AlsaOutput::Enable()
{
	pcm_export.Construct();
}

inline void
AlsaOutput::Disable()
{
	pcm_export.Destruct();
}

static bool
alsa_test_default_device()
{
	snd_pcm_t *handle;

	int ret = snd_pcm_open(&handle, default_device,
			       SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret) {
		FormatError(alsa_output_domain,
			    "Error opening default ALSA device: %s",
			    snd_strerror(-ret));
		return false;
	} else
		snd_pcm_close(handle);

	return true;
}

/**
 * Convert MPD's #SampleFormat enum to libasound's snd_pcm_format_t
 * enum.  Returns SND_PCM_FORMAT_UNKNOWN if there is no according ALSA
 * PCM format.
 */
gcc_const
static snd_pcm_format_t
ToAlsaPcmFormat(SampleFormat sample_format) noexcept
{
	switch (sample_format) {
	case SampleFormat::UNDEFINED:
		return SND_PCM_FORMAT_UNKNOWN;

	case SampleFormat::DSD:
#ifdef HAVE_ALSA_DSD
		return SND_PCM_FORMAT_DSD_U8;
#else
		return SND_PCM_FORMAT_UNKNOWN;
#endif

	case SampleFormat::S8:
		return SND_PCM_FORMAT_S8;

	case SampleFormat::S16:
		return SND_PCM_FORMAT_S16;

	case SampleFormat::S24_P32:
		return SND_PCM_FORMAT_S24;

	case SampleFormat::S32:
		return SND_PCM_FORMAT_S32;

	case SampleFormat::FLOAT:
		return SND_PCM_FORMAT_FLOAT;
	}

	assert(false);
	gcc_unreachable();
}

/**
 * Determine the byte-swapped PCM format.  Returns
 * SND_PCM_FORMAT_UNKNOWN if the format cannot be byte-swapped.
 */
static snd_pcm_format_t
ByteSwapAlsaPcmFormat(snd_pcm_format_t fmt) noexcept
{
	switch (fmt) {
	case SND_PCM_FORMAT_S16_LE: return SND_PCM_FORMAT_S16_BE;
	case SND_PCM_FORMAT_S24_LE: return SND_PCM_FORMAT_S24_BE;
	case SND_PCM_FORMAT_S32_LE: return SND_PCM_FORMAT_S32_BE;
	case SND_PCM_FORMAT_S16_BE: return SND_PCM_FORMAT_S16_LE;
	case SND_PCM_FORMAT_S24_BE: return SND_PCM_FORMAT_S24_LE;

	case SND_PCM_FORMAT_S24_3BE:
		return SND_PCM_FORMAT_S24_3LE;

	case SND_PCM_FORMAT_S24_3LE:
		return SND_PCM_FORMAT_S24_3BE;

	case SND_PCM_FORMAT_S32_BE: return SND_PCM_FORMAT_S32_LE;

#ifdef HAVE_ALSA_DSD_U32
	case SND_PCM_FORMAT_DSD_U16_LE:
		return SND_PCM_FORMAT_DSD_U16_BE;

	case SND_PCM_FORMAT_DSD_U16_BE:
		return SND_PCM_FORMAT_DSD_U16_LE;

	case SND_PCM_FORMAT_DSD_U32_LE:
		return SND_PCM_FORMAT_DSD_U32_BE;

	case SND_PCM_FORMAT_DSD_U32_BE:
		return SND_PCM_FORMAT_DSD_U32_LE;
#endif

	default: return SND_PCM_FORMAT_UNKNOWN;
	}
}

/**
 * Check if there is a "packed" version of the give PCM format.
 * Returns SND_PCM_FORMAT_UNKNOWN if not.
 */
static snd_pcm_format_t
PackAlsaPcmFormat(snd_pcm_format_t fmt)
{
	switch (fmt) {
	case SND_PCM_FORMAT_S24_LE:
		return SND_PCM_FORMAT_S24_3LE;

	case SND_PCM_FORMAT_S24_BE:
		return SND_PCM_FORMAT_S24_3BE;

	default:
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

/**
 * Attempts to configure the specified sample format.  On failure,
 * fall back to the packed version.
 */
static int
AlsaTryFormatOrPacked(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		      snd_pcm_format_t fmt, PcmExport::Params &params)
{
	int err = snd_pcm_hw_params_set_format(pcm, hwparams, fmt);
	if (err == 0)
		params.pack24 = false;

	if (err != -EINVAL)
		return err;

	fmt = PackAlsaPcmFormat(fmt);
	if (fmt == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	err = snd_pcm_hw_params_set_format(pcm, hwparams, fmt);
	if (err == 0)
		params.pack24 = true;

	return err;
}

/**
 * Attempts to configure the specified sample format, and tries the
 * reversed host byte order if was not supported.
 */
static int
AlsaTryFormatOrByteSwap(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
			snd_pcm_format_t fmt,
			PcmExport::Params &params)
{
	int err = AlsaTryFormatOrPacked(pcm, hwparams, fmt, params);
	if (err == 0)
		params.reverse_endian = false;

	if (err != -EINVAL)
		return err;

	fmt = ByteSwapAlsaPcmFormat(fmt);
	if (fmt == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	err = AlsaTryFormatOrPacked(pcm, hwparams, fmt, params);
	if (err == 0)
		params.reverse_endian = true;

	return err;
}

/**
 * Attempts to configure the specified sample format.  On DSD_U8
 * failure, attempt to switch to DSD_U32 or DSD_U16.
 */
static int
AlsaTryFormatDsd(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		 snd_pcm_format_t fmt, PcmExport::Params &params)
{
	int err = AlsaTryFormatOrByteSwap(pcm, hwparams, fmt, params);

#if defined(ENABLE_DSD) && defined(HAVE_ALSA_DSD_U32)
	if (err == 0) {
		params.dsd_u16 = false;
		params.dsd_u32 = false;
	}

	if (err == -EINVAL && fmt == SND_PCM_FORMAT_DSD_U8) {
		/* attempt to switch to DSD_U32 */
		fmt = IsLittleEndian()
			? SND_PCM_FORMAT_DSD_U32_LE
			: SND_PCM_FORMAT_DSD_U32_BE;
		err = AlsaTryFormatOrByteSwap(pcm, hwparams, fmt, params);
		if (err == 0)
			params.dsd_u32 = true;
		else
			fmt = SND_PCM_FORMAT_DSD_U8;
	}

	if (err == -EINVAL && fmt == SND_PCM_FORMAT_DSD_U8) {
		/* attempt to switch to DSD_U16 */
		fmt = IsLittleEndian()
			? SND_PCM_FORMAT_DSD_U16_LE
			: SND_PCM_FORMAT_DSD_U16_BE;
		err = AlsaTryFormatOrByteSwap(pcm, hwparams, fmt, params);
		if (err == 0)
			params.dsd_u16 = true;
		else
			fmt = SND_PCM_FORMAT_DSD_U8;
	}
#endif

	return err;
}

static int
AlsaTryFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
	      SampleFormat sample_format,
	      PcmExport::Params &params)
{
	snd_pcm_format_t alsa_format = ToAlsaPcmFormat(sample_format);
	if (alsa_format == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	return AlsaTryFormatDsd(pcm, hwparams, alsa_format, params);
}

/**
 * Configure a sample format, and probe other formats if that fails.
 */
static int
AlsaSetupFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		AudioFormat &audio_format,
		PcmExport::Params &params)
{
	/* try the input format first */

	int err = AlsaTryFormat(pcm, hwparams, audio_format.format, params);

	/* if unsupported by the hardware, try other formats */

	static constexpr SampleFormat probe_formats[] = {
		SampleFormat::S24_P32,
		SampleFormat::S32,
		SampleFormat::S16,
		SampleFormat::S8,
		SampleFormat::UNDEFINED,
	};

	for (unsigned i = 0;
	     err == -EINVAL && probe_formats[i] != SampleFormat::UNDEFINED;
	     ++i) {
		const SampleFormat mpd_format = probe_formats[i];
		if (mpd_format == audio_format.format)
			continue;

		err = AlsaTryFormat(pcm, hwparams, mpd_format, params);
		if (err == 0)
			audio_format.format = mpd_format;
	}

	return err;
}

/**
 * Wrapper for snd_pcm_hw_params().
 *
 * @param buffer_time the configured buffer time, or 0 if not configured
 * @param period_time the configured period time, or 0 if not configured
 * @param audio_format an #AudioFormat to be configured (or modified)
 * by this function
 * @param params to be modified by this function
 */
static void
AlsaSetupHw(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
	    unsigned buffer_time, unsigned period_time,
	    AudioFormat &audio_format, PcmExport::Params &params)
{
	int err;
	unsigned retry = MPD_ALSA_RETRY_NR;
	unsigned int period_time_ro = period_time;

configure_hw:
	/* configure HW params */
	err = snd_pcm_hw_params_any(pcm, hwparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_any() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_hw_params_set_access(pcm, hwparams,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_set_access() failed: %s",
					 snd_strerror(-err));

	err = AlsaSetupFormat(pcm, hwparams, audio_format, params);
	if (err < 0)
		throw FormatRuntimeError("Failed to configure format %s: %s",
					 sample_format_to_string(audio_format.format),
					 snd_strerror(-err));

	unsigned int channels = audio_format.channels;
	err = snd_pcm_hw_params_set_channels_near(pcm, hwparams,
						  &channels);
	if (err < 0)
		throw FormatRuntimeError("Failed to configure %i channels: %s",
					 (int)audio_format.channels,
					 snd_strerror(-err));

	audio_format.channels = (int8_t)channels;

	const unsigned requested_sample_rate =
		params.CalcOutputSampleRate(audio_format.sample_rate);
	unsigned output_sample_rate = requested_sample_rate;

	err = snd_pcm_hw_params_set_rate_near(pcm, hwparams,
					      &output_sample_rate, nullptr);
	if (err < 0)
		throw FormatRuntimeError("Failed to configure sample rate %u Hz: %s",
					 requested_sample_rate,
					 snd_strerror(-err));

	if (output_sample_rate == 0)
		throw FormatRuntimeError("Failed to configure sample rate %u Hz",
					 audio_format.sample_rate);

	if (output_sample_rate != requested_sample_rate)
		audio_format.sample_rate = params.CalcInputSampleRate(output_sample_rate);

	snd_pcm_uframes_t buffer_size_min, buffer_size_max;
	snd_pcm_hw_params_get_buffer_size_min(hwparams, &buffer_size_min);
	snd_pcm_hw_params_get_buffer_size_max(hwparams, &buffer_size_max);
	unsigned buffer_time_min, buffer_time_max;
	snd_pcm_hw_params_get_buffer_time_min(hwparams, &buffer_time_min, 0);
	snd_pcm_hw_params_get_buffer_time_max(hwparams, &buffer_time_max, 0);
	FormatDebug(alsa_output_domain, "buffer: size=%u..%u time=%u..%u",
		    (unsigned)buffer_size_min, (unsigned)buffer_size_max,
		    buffer_time_min, buffer_time_max);

	snd_pcm_uframes_t period_size_min, period_size_max;
	snd_pcm_hw_params_get_period_size_min(hwparams, &period_size_min, 0);
	snd_pcm_hw_params_get_period_size_max(hwparams, &period_size_max, 0);
	unsigned period_time_min, period_time_max;
	snd_pcm_hw_params_get_period_time_min(hwparams, &period_time_min, 0);
	snd_pcm_hw_params_get_period_time_max(hwparams, &period_time_max, 0);
	FormatDebug(alsa_output_domain, "period: size=%u..%u time=%u..%u",
		    (unsigned)period_size_min, (unsigned)period_size_max,
		    period_time_min, period_time_max);

	if (buffer_time > 0) {
		err = snd_pcm_hw_params_set_buffer_time_near(pcm, hwparams,
							     &buffer_time, nullptr);
		if (err < 0)
			throw FormatRuntimeError("snd_pcm_hw_params_set_buffer_time_near() failed: %s",
						 snd_strerror(-err));
	} else {
		err = snd_pcm_hw_params_get_buffer_time(hwparams, &buffer_time,
							nullptr);
		if (err < 0)
			buffer_time = 0;
	}

	if (period_time_ro == 0 && buffer_time >= 10000) {
		period_time_ro = period_time = buffer_time / 4;

		FormatDebug(alsa_output_domain,
			    "default period_time = buffer_time/4 = %u/4 = %u",
			    buffer_time, period_time);
	}

	if (period_time_ro > 0) {
		period_time = period_time_ro;
		err = snd_pcm_hw_params_set_period_time_near(pcm, hwparams,
							     &period_time, nullptr);
		if (err < 0)
			throw FormatRuntimeError("snd_pcm_hw_params_set_period_time_near() failed: %s",
						 snd_strerror(-err));
	}

	err = snd_pcm_hw_params(pcm, hwparams);
	if (err == -EPIPE && --retry > 0 && period_time_ro > 0) {
		period_time_ro = period_time_ro >> 1;
		goto configure_hw;
	} else if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params() failed: %s",
					 snd_strerror(-err));
	if (retry != MPD_ALSA_RETRY_NR)
		FormatDebug(alsa_output_domain,
			    "ALSA period_time set to %d", period_time);
}

/**
 * Wrapper for snd_pcm_sw_params().
 */
static void
AlsaSetupSw(snd_pcm_t *pcm, snd_pcm_uframes_t start_threshold,
	    snd_pcm_uframes_t avail_min)
{
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sw_params_alloca(&swparams);

	int err = snd_pcm_sw_params_current(pcm, swparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params_current() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_sw_params_set_start_threshold(pcm, swparams,
						    start_threshold);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params_set_start_threshold() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_sw_params_set_avail_min(pcm, swparams, avail_min);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params_set_avail_min() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_sw_params(pcm, swparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params() failed: %s",
					 snd_strerror(-err));
}

inline void
AlsaOutput::Setup(AudioFormat &audio_format,
		  PcmExport::Params &params)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_hw_params_alloca(&hwparams);

	AlsaSetupHw(pcm, hwparams,
		    buffer_time, period_time,
		    audio_format, params);

	snd_pcm_format_t format;
	if (snd_pcm_hw_params_get_format(hwparams, &format) == 0)
		FormatDebug(alsa_output_domain,
			    "format=%s (%s)", snd_pcm_format_name(format),
			    snd_pcm_format_description(format));

	snd_pcm_uframes_t alsa_buffer_size;
	int err = snd_pcm_hw_params_get_buffer_size(hwparams, &alsa_buffer_size);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_get_buffer_size() failed: %s",
					 snd_strerror(-err));

	snd_pcm_uframes_t alsa_period_size;
	err = snd_pcm_hw_params_get_period_size(hwparams, &alsa_period_size,
						nullptr);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_get_period_size() failed: %s",
					 snd_strerror(-err));

	AlsaSetupSw(pcm, alsa_buffer_size - alsa_period_size,
		    alsa_period_size);

	FormatDebug(alsa_output_domain, "buffer_size=%u period_size=%u",
		    (unsigned)alsa_buffer_size, (unsigned)alsa_period_size);

	if (alsa_period_size == 0)
		/* this works around a SIGFPE bug that occurred when
		   an ALSA driver indicated period_size==0; this
		   caused a division by zero in alsa_play().  By using
		   the fallback "1", we make sure that this won't
		   happen again. */
		alsa_period_size = 1;

	period_frames = alsa_period_size;
	period_position = 0;

	silence = new uint8_t[snd_pcm_frames_to_bytes(pcm, alsa_period_size)];
	snd_pcm_format_set_silence(format, silence,
				   alsa_period_size * audio_format.channels);

}

#ifdef ENABLE_DSD

inline void
AlsaOutput::SetupDop(const AudioFormat audio_format,
		     PcmExport::Params &params)
{
	assert(dop);
	assert(audio_format.format == SampleFormat::DSD);

	/* pass 24 bit to AlsaSetup() */

	AudioFormat dop_format = audio_format;
	dop_format.format = SampleFormat::S24_P32;

	const AudioFormat check = dop_format;

	Setup(dop_format, params);

	/* if the device allows only 32 bit, shift all DoP
	   samples left by 8 bit and leave the lower 8 bit cleared;
	   the DSD-over-USB documentation does not specify whether
	   this is legal, but there is anecdotical evidence that this
	   is possible (and the only option for some devices) */
	params.shift8 = dop_format.format == SampleFormat::S32;
	if (dop_format.format == SampleFormat::S32)
		dop_format.format = SampleFormat::S24_P32;

	if (dop_format != check) {
		/* no bit-perfect playback, which is required
		   for DSD over USB */
		delete[] silence;
		throw std::runtime_error("Failed to configure DSD-over-PCM");
	}
}

#endif

inline void
AlsaOutput::SetupOrDop(AudioFormat &audio_format, PcmExport::Params &params)
{
#ifdef ENABLE_DSD
	std::exception_ptr dop_error;
	if (dop && audio_format.format == SampleFormat::DSD) {
		try {
			params.dop = true;
			SetupDop(audio_format, params);
			return;
		} catch (...) {
			dop_error = std::current_exception();
			params.dop = false;
		}
	}

	try {
#endif
		Setup(audio_format, params);
#ifdef ENABLE_DSD
	} catch (...) {
		if (dop_error)
			/* if DoP was attempted, prefer returning the
			   original DoP error instead of the fallback
			   error */
			std::rethrow_exception(dop_error);
		else
			throw;
	}
#endif
}

inline void
AlsaOutput::Open(AudioFormat &audio_format)
{
	int err = snd_pcm_open(&pcm, GetDevice(),
			       SND_PCM_STREAM_PLAYBACK, mode);
	if (err < 0)
		throw FormatRuntimeError("Failed to open ALSA device \"%s\": %s",
					 GetDevice(), snd_strerror(err));

	FormatDebug(alsa_output_domain, "opened %s type=%s",
		    snd_pcm_name(pcm),
		    snd_pcm_type_name(snd_pcm_type(pcm)));

	PcmExport::Params params;
	params.alsa_channel_order = true;

	try {
		SetupOrDop(audio_format, params);
	} catch (...) {
		snd_pcm_close(pcm);
		std::throw_with_nested(FormatRuntimeError("Error opening ALSA device \"%s\"",
							  GetDevice()));
	}

#ifdef ENABLE_DSD
	if (params.dop)
		FormatDebug(alsa_output_domain, "DoP (DSD over PCM) enabled");
#endif

	pcm_export->Open(audio_format.format,
			 audio_format.channels,
			 params);

	in_frame_size = audio_format.GetFrameSize();
	out_frame_size = pcm_export->GetFrameSize(audio_format);

	must_prepare = false;
}

inline int
AlsaOutput::Recover(int err)
{
	if (err == -EPIPE) {
		FormatDebug(alsa_output_domain,
			    "Underrun on ALSA device \"%s\"",
			    GetDevice());
	} else if (err == -ESTRPIPE) {
		FormatDebug(alsa_output_domain,
			    "ALSA device \"%s\" was suspended",
			    GetDevice());
	}

	switch (snd_pcm_state(pcm)) {
	case SND_PCM_STATE_PAUSED:
		err = snd_pcm_pause(pcm, /* disable */ 0);
		break;
	case SND_PCM_STATE_SUSPENDED:
		err = snd_pcm_resume(pcm);
		if (err == -EAGAIN)
			return 0;
		/* fall-through to snd_pcm_prepare: */
#if GCC_CHECK_VERSION(7,0)
		[[fallthrough]];
#endif
	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_XRUN:
		period_position = 0;
		err = snd_pcm_prepare(pcm);
		break;
	case SND_PCM_STATE_DISCONNECTED:
		break;
	/* this is no error, so just keep running */
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
	case SND_PCM_STATE_DRAINING:
		err = 0;
		break;

	default:
		/* this default case is just here to work around
		   -Wswitch due to SND_PCM_STATE_PRIVATE1 (libasound
		   1.1.6) */
		break;
	}

	return err;
}

inline void
AlsaOutput::Drain()
{
	if (snd_pcm_state(pcm) != SND_PCM_STATE_RUNNING)
		return;

	if (period_position > 0) {
		/* generate some silence to finish the partial
		   period */
		snd_pcm_uframes_t nframes =
			period_frames - period_position;
		WriteSilence(nframes);
	}

	snd_pcm_drain(pcm);

	period_position = 0;
}

inline void
AlsaOutput::Cancel()
{
	period_position = 0;
	must_prepare = true;

	snd_pcm_drop(pcm);

	pcm_export->Reset();
}

inline void
AlsaOutput::Close()
{
	snd_pcm_close(pcm);
	delete[] silence;
}

inline size_t
AlsaOutput::PlayRaw(ConstBuffer<void> data)
{
	if (data.IsEmpty())
		return 0;

	assert(data.size % out_frame_size == 0);

	const size_t n_frames = data.size / out_frame_size;
	assert(n_frames > 0);

	while (true) {
		const auto frames_written = snd_pcm_writei(pcm, data.data,
							   n_frames);
		if (frames_written > 0) {
			period_position = (period_position + frames_written)
				% period_frames;

			return frames_written * out_frame_size;
		}

		if (frames_written < 0 && frames_written != -EAGAIN &&
		    frames_written != -EINTR &&
		    Recover(frames_written) < 0)
			throw FormatRuntimeError("snd_pcm_writei() failed: %s",
						 snd_strerror(-frames_written));
	}

}

inline size_t
AlsaOutput::Play(const void *chunk, size_t size)
{
	assert(size > 0);
	assert(size % in_frame_size == 0);

	if (must_prepare) {
		must_prepare = false;

		int err = snd_pcm_prepare(pcm);
		if (err < 0)
			throw FormatRuntimeError("snd_pcm_prepare() failed: %s",
						 snd_strerror(-err));
	}

	const auto e = pcm_export->Export({chunk, size});
	if (e.size == 0)
		/* the DoP (DSD over PCM) filter converts two frames
		   at a time and ignores the last odd frame; if there
		   was only one frame (e.g. the last frame in the
		   file), the result is empty; to avoid an endless
		   loop, bail out here, and pretend the one frame has
		   been played */
		return size;

	const size_t bytes_written = PlayRaw(e);
	return pcm_export->CalcSourceSize(bytes_written);
}

typedef AudioOutputWrapper<AlsaOutput> Wrapper;

const struct AudioOutputPlugin alsa_output_plugin = {
	"alsa",
	alsa_test_default_device,
	&Wrapper::Init,
	&Wrapper::Finish,
	&Wrapper::Enable,
	&Wrapper::Disable,
	&Wrapper::Open,
	&Wrapper::Close,
	nullptr,
	nullptr,
	&Wrapper::Play,
	&Wrapper::Drain,
	&Wrapper::Cancel,
	nullptr,

	&alsa_mixer_plugin,
};

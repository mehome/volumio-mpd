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
#include "test_pcm_all.hxx"
#include "pcm/PcmExport.hxx"
#include "pcm/Traits.hxx"
#include "system/ByteOrder.hxx"
#include "util/ConstBuffer.hxx"

#include <string.h>

void
PcmExportTest::TestShift8()
{
	static constexpr int32_t src[] = { 0x0, 0x1, 0x100, 0x10000, 0xffffff };
	static constexpr uint32_t expected[] = { 0x0, 0x100, 0x10000, 0x1000000, 0xffffff00 };

	PcmExport::Params params;
	params.shift8 = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(42u), 42u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(42u), 42u);

	PcmExport e;
	e.Open(SampleFormat::S24_P32, 2, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

void
PcmExportTest::TestPack24()
{
	static constexpr int32_t src[] = { 0x0, 0x1, 0x100, 0x10000, 0xffffff };

	static constexpr uint8_t expected_be[] = {
		0, 0, 0x0,
		0, 0, 0x1,
		0, 0x1, 0x00,
		0x1, 0x00, 0x00,
		0xff, 0xff, 0xff,
	};

	static constexpr uint8_t expected_le[] = {
		0, 0, 0x0,
		0x1, 0, 0,
		0x00, 0x1, 0,
		0, 0x00, 0x01,
		0xff, 0xff, 0xff,
	};

	static constexpr size_t expected_size = sizeof(expected_be);
	static const uint8_t *const expected = IsBigEndian()
		? expected_be : expected_le;

	PcmExport::Params params;
	params.pack24 = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(42u), 42u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(42u), 42u);

	PcmExport e;
	e.Open(SampleFormat::S24_P32, 2, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(expected_size, dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

void
PcmExportTest::TestReverseEndian()
{
	static constexpr uint8_t src[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
	};

	static constexpr uint8_t expected2[] = {
		2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11
	};

	static constexpr uint8_t expected4[] = {
		4, 3, 2, 1, 8, 7, 6, 5, 12, 11, 10, 9,
	};

	PcmExport::Params params;
	params.reverse_endian = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(42u), 42u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(42u), 42u);

	PcmExport e;
	e.Open(SampleFormat::S8, 2, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(src), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, src, dest.size) == 0);

	e.Open(SampleFormat::S16, 2, params);
	dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected2), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected2, dest.size) == 0);

	e.Open(SampleFormat::S32, 2, params);
	dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected4), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected4, dest.size) == 0);
}

#ifdef ENABLE_DSD

void
PcmExportTest::TestDsdU16()
{
	static constexpr uint8_t src[] = {
		0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef,
		0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88,
	};

	static constexpr uint16_t expected[] = {
		0x0145, 0x2367,
		0x89cd, 0xabef,
		0x1133, 0x2244,
		0x5577, 0x6688,
	};

	PcmExport::Params params;
	params.dsd_u16 = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(705600u), 352800u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(352800u), 705600u);

	PcmExport e;
	e.Open(SampleFormat::DSD, 2, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

void
PcmExportTest::TestDsdU32()
{
	static constexpr uint8_t src[] = {
		0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef,
		0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88,
	};

	static constexpr uint32_t expected[] = {
		0x014589cd,
		0x2367abef,
		0x11335577,
		0x22446688,
	};

	PcmExport::Params params;
	params.dsd_u32 = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(705600u), 176400u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(176400u), 705600u);

	PcmExport e;
	e.Open(SampleFormat::DSD, 2, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

void
PcmExportTest::TestDop()
{
	static constexpr uint8_t src[] = {
		0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef,
	};

	static constexpr uint32_t expected[] = {
		0xff050145,
		0xff052367,
		0xfffa89cd,
		0xfffaabef,
	};

	PcmExport::Params params;
	params.dop = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(705600u), 352800u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(352800u), 705600u);

	PcmExport e;
	e.Open(SampleFormat::DSD, 2, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

#endif

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
TestAlsaChannelOrder51()
{
	typedef typename Traits::value_type value_type;

	static constexpr value_type src[] = {
		0, 1, 2, 3, 4, 5,
		6, 7, 8, 9, 10, 11,
	};

	static constexpr value_type expected[] = {
		0, 1, 4, 5, 2, 3,
		6, 7, 10, 11, 8, 9,
	};

	PcmExport::Params params;
	params.alsa_channel_order = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(42u), 42u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(42u), 42u);

	PcmExport e;
	e.Open(F, 6, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
TestAlsaChannelOrder71()
{
	typedef typename Traits::value_type value_type;

	static constexpr value_type src[] = {
		0, 1, 2, 3, 4, 5, 6, 7,
		8, 9, 10, 11, 12, 13, 14, 15,
	};

	static constexpr value_type expected[] = {
		0, 1, 4, 5, 2, 3, 6, 7,
		8, 9, 12, 13, 10, 11, 14, 15,
	};

	PcmExport::Params params;
	params.alsa_channel_order = true;

	CPPUNIT_ASSERT_EQUAL(params.CalcOutputSampleRate(42u), 42u);
	CPPUNIT_ASSERT_EQUAL(params.CalcInputSampleRate(42u), 42u);

	PcmExport e;
	e.Open(F, 8, params);

	auto dest = e.Export({src, sizeof(src)});
	CPPUNIT_ASSERT_EQUAL(sizeof(expected), dest.size);
	CPPUNIT_ASSERT(memcmp(dest.data, expected, dest.size) == 0);
}

void
PcmExportTest::TestAlsaChannelOrder()
{
	TestAlsaChannelOrder51<SampleFormat::S16>();
	TestAlsaChannelOrder71<SampleFormat::S16>();
	TestAlsaChannelOrder51<SampleFormat::S32>();
	TestAlsaChannelOrder71<SampleFormat::S32>();
}

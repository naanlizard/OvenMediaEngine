#include <gtest/gtest.h>

#include <limits>

#include "llhls_session.h"

// Friended by LLHlsSession, so it stays at global scope
class LLHlsFileName : public testing::Test
{
protected:
	using RequestType = LLHlsSession::RequestType;

	bool Parse(const ov::String &name)
	{
		file = name;
		return LLHlsSession::ParseFileName(file, type, track, segment, part, version, key);
	}

	// `key` views into `file`
	ov::String file;
	RequestType type{};
	int32_t track = 0;
	int64_t segment = 0, part = 0, version = 0;
	std::string_view key;
};

TEST_F(LLHlsFileName, RejectsOverflow)
{
	for (const char *token : {"9223372036854775808", "-9223372036854775809",
							  "18446744073709551616", "9999999999999999999999999999999999999999"})
	{
		SCOPED_TRACE(token);
		EXPECT_FALSE(Parse(ov::String::FormatString("seg_%s_0_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("seg_1_%s_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("part_1_%s_0_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("part_1_0_%s_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("init_1_video_key_v%s_llhls.m4s", token)));
	}
}

TEST_F(LLHlsFileName, RejectsPartialAndEmptyTokens)
{
	for (const char *token : {"12tail", "tail", "", "+", "-", "+-1", " 1", "1 "})
	{
		SCOPED_TRACE(token);
		EXPECT_FALSE(Parse(ov::String::FormatString("chunklist_%s_video_key_llhls.m3u8", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("init_%s_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("seg_1_%s_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("part_1_%s_0_video_key_llhls.vtt", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("part_1_0_%s_video_key_llhls.m4s", token)));
		EXPECT_FALSE(Parse(ov::String::FormatString("init_1_video_key_v%s_llhls.m4s", token)));
	}
}

TEST_F(LLHlsFileName, RejectsOutOfRangeTrackAndVersion)
{
	for (const char *token : {"2147483648", "-2147483649", "4294967297"})
	{
		SCOPED_TRACE(token);
		EXPECT_FALSE(Parse(ov::String::FormatString("chunklist_%s_video_key_llhls.m3u8", token)));
	}
	EXPECT_FALSE(Parse("init_1_video_key_v4294967296_llhls.m4s"));
	EXPECT_FALSE(Parse("init_1_video_key_v-1_llhls.m4s"));
}

TEST_F(LLHlsFileName, AcceptsValidValuesAndBoundaries)
{
	ASSERT_TRUE(Parse("chunklist_2147483647_video_key_llhls.m3u8"));
	EXPECT_EQ(type, RequestType::Chunklist);
	EXPECT_EQ(track, std::numeric_limits<int32_t>::max());
	ASSERT_TRUE(Parse("seg_-2147483648_9223372036854775807_video_key_llhls.m4s"));
	EXPECT_EQ(type, RequestType::Segment);
	EXPECT_EQ(track, std::numeric_limits<int32_t>::min());
	EXPECT_EQ(segment, std::numeric_limits<int64_t>::max());
	EXPECT_EQ(key, "key");
	ASSERT_TRUE(Parse("part_+1_-9223372036854775808_9223372036854775807_video_key_llhls.vtt"));
	EXPECT_EQ(type, RequestType::PartialSegment);
	EXPECT_EQ(track, 1);
	EXPECT_EQ(segment, std::numeric_limits<int64_t>::min());
	EXPECT_EQ(part, std::numeric_limits<int64_t>::max());
	ASSERT_TRUE(Parse("part_1_+00042_0_video_key_llhls.m4s"));
	EXPECT_EQ(segment, 42);
	EXPECT_EQ(part, 0);
	ASSERT_TRUE(Parse("init_1_video_key_v4294967295_llhls.m4s"));
	EXPECT_EQ(type, RequestType::InitializationSegment);
	EXPECT_EQ(version, std::numeric_limits<uint32_t>::max());
	ASSERT_TRUE(Parse("init_1_video_key_v0_llhls.m4s"));
	EXPECT_EQ(version, 0);
	ASSERT_TRUE(Parse("init_1_video_key_llhls.m4s"));
	EXPECT_EQ(version, -1);
	ASSERT_TRUE(Parse("llhls.m3u8"));
	EXPECT_EQ(type, RequestType::Playlist);
}

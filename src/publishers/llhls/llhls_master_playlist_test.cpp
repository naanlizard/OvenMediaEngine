#include <gtest/gtest.h>

#include "llhls_master_playlist.h"

namespace
{
	std::shared_ptr<MediaTrack> CreateVideoTrack(uint32_t id, int32_t width, int32_t height, int32_t bitrate)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(id);
		track->SetMediaType(cmn::MediaType::Video);
		track->SetCodecId(cmn::MediaCodecId::H264);
		track->SetResolution(width, height);
		track->SetFrameRateByConfig(30.0);
		track->SetBitrateByConfig(bitrate);
		return track;
	}

	void AddCandidate(LLHlsMasterPlaylist &master, const ov::String &group_id, cmn::MediaType type, const std::shared_ptr<MediaTrack> &track, const ov::String &uri)
	{
		LLHlsMasterPlaylist::MediaInfo info;
		info._group_id = group_id;
		info._type = type;
		info._name = group_id;
		info._uri = uri;
		info._track = track;
		master.AddMediaCandidate(info);
	}

	int CountOccurrences(const ov::String &haystack, const char *needle)
	{
		int count = 0;
		auto index = haystack.IndexOf(needle);
		while (index != -1)
		{
			count++;
			index = haystack.IndexOf(needle, index + 1);
		}
		return count;
	}

	// Two video-only variants, the way LLHlsStream::CreateMasterPlaylist registers them
	void AddTwoVideoVariants(LLHlsMasterPlaylist &master)
	{
		AddCandidate(master, "video_720", cmn::MediaType::Video, CreateVideoTrack(1, 1280, 720, 2500000), "chunklist_1_video_llhls.m3u8");
		AddCandidate(master, "video_480", cmn::MediaType::Video, CreateVideoTrack(2, 854, 480, 1200000), "chunklist_2_video_llhls.m3u8");
		master.SetTrackCodecs(1, "avc1.64001f");
		master.SetTrackCodecs(2, "avc1.4d401e");
	}
} // namespace

TEST(LLHlsMasterPlaylist, ScoreOnEveryVariantWhenAllScored)
{
	LLHlsMasterPlaylist master;
	AddTwoVideoVariants(master);

	ASSERT_TRUE(master.AddStreamInfo("video_720", 0, "", "", 1.0));
	ASSERT_TRUE(master.AddStreamInfo("video_480", 0, "", "", 0.25));

	auto playlist = master.ToString("", false, true);

	// Each variant line carries its own SCORE, rendered to three decimals
	EXPECT_EQ(CountOccurrences(playlist, "#EXT-X-STREAM-INF:"), 2);
	EXPECT_EQ(CountOccurrences(playlist, ",SCORE="), 2);
	EXPECT_NE(playlist.IndexOf("RESOLUTION=1280x720,FRAME-RATE=30.000,CODECS=\"avc1.64001f\",SCORE=1.000\n"), -1);
	EXPECT_NE(playlist.IndexOf("RESOLUTION=854x480,FRAME-RATE=30.000,CODECS=\"avc1.4d401e\",SCORE=0.250\n"), -1);
}

TEST(LLHlsMasterPlaylist, ScoreOmittedWhenOnlySomeVariantsScored)
{
	LLHlsMasterPlaylist master;
	AddTwoVideoVariants(master);

	// HLS SCORE is all-or-nothing: one unscored variant suppresses it everywhere
	ASSERT_TRUE(master.AddStreamInfo("video_720", 0, "", "", 1.0));
	ASSERT_TRUE(master.AddStreamInfo("video_480", 0, "", ""));

	auto playlist = master.ToString("", false, true);

	EXPECT_EQ(CountOccurrences(playlist, "#EXT-X-STREAM-INF:"), 2);
	EXPECT_EQ(playlist.IndexOf("SCORE="), -1);
}

TEST(LLHlsMasterPlaylist, NoScoreWhenUnscored)
{
	LLHlsMasterPlaylist master;
	AddTwoVideoVariants(master);

	ASSERT_TRUE(master.AddStreamInfo("video_720", 0, "", ""));
	ASSERT_TRUE(master.AddStreamInfo("video_480", 0, "", ""));

	auto playlist = master.ToString("", false, true);

	EXPECT_EQ(CountOccurrences(playlist, "#EXT-X-STREAM-INF:"), 2);
	EXPECT_EQ(playlist.IndexOf("SCORE="), -1);
}

TEST(LLHlsMasterPlaylist, SmallestScoreTheConfigAcceptsPrintsPositive)
{
	LLHlsMasterPlaylist master;
	AddTwoVideoVariants(master);

	ASSERT_TRUE(master.AddStreamInfo("video_720", 0, "", "", 1.0));
	ASSERT_TRUE(master.AddStreamInfo("video_480", 0, "", "", 0.0005));

	auto playlist = master.ToString("", false, true);

	EXPECT_NE(playlist.IndexOf("CODECS=\"avc1.4d401e\",SCORE=0.001\n"), -1);
}

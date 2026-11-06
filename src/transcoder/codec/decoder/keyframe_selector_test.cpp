//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include "keyframe_selector.h"

// At 0.1 fps one output slot spans 10 s, centred on multiples of 10 s (1/1000 timebase).

namespace
{
	const cmn::Timebase kTimebase(1, 1000);
	constexpr int64_t kMaxGap = 15000;

	std::shared_ptr<MediaPacket> Key(int64_t pts_ms)
	{
		uint8_t byte = 0x00;
		return std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, &byte, 1, pts_ms, pts_ms, 0,
											 MediaPacketFlag::Key, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
	}

	// The selector with one image output per rate; images[i] is the count output i has produced
	struct Selector
	{
		explicit Selector(std::vector<double> framerates)
			: images(framerates.size(), 0)
		{
			std::vector<KeyframeSelector::ImageOutput> outputs;
			for (size_t i = 0; i < framerates.size(); i++)
			{
				outputs.push_back({framerates[i], [this, i]() { return images[i]; }});
			}
			selector.Configure(kTimebase, std::move(outputs), kMaxGap);
		}

		KeyframeSelector::Decision Feed(int64_t pts_ms, bool can_hold = true)
		{
			return selector.OnKeyframe(Key(pts_ms), can_hold);
		}

		// Feeds a keyframe, then every output produces an image
		KeyframeSelector::Decision FeedAndProduce(int64_t pts_ms)
		{
			auto decision = Feed(pts_ms);
			for (auto &count : images)
			{
				count++;
			}
			return decision;
		}

		void Reconfigure()
		{
			selector.Configure(kTimebase, {{0.1, []() { return uint64_t(0); }}}, kMaxGap);
		}

		KeyframeSelector selector;
		std::vector<uint64_t> images;
	};
}  // namespace

TEST(KeyframeSelector, WithoutImageOutputsItIsNotEnabled)
{
	Selector selector({});
	EXPECT_FALSE(selector.selector.IsEnabled());
}

TEST(KeyframeSelector, TheFirstKeyframeIsDecodedAndTheNextOnesAreHeld)
{
	Selector selector({0.1});
	ASSERT_TRUE(selector.selector.IsEnabled());

	auto decision = selector.Feed(0);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);

	EXPECT_TRUE(selector.Feed(2000).hold);
}

TEST(KeyframeSelector, NothingIsHeldWithoutAPictureToStandIn)
{
	Selector selector({0.1});
	selector.Feed(0);

	auto decision = selector.Feed(2000, false);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);
}

TEST(KeyframeSelector, OnlyTheLastKeyframeOfASlotIsDecoded)
{
	Selector selector({0.1});
	selector.Feed(0);
	EXPECT_TRUE(selector.Feed(2000).hold);

	// 4000 takes the slot of 2000, which the filter would drop
	auto k4		  = Key(4000);
	auto decision = selector.selector.OnKeyframe(k4, true);
	EXPECT_TRUE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);

	// 6000 opens slot 1: 4000 was the last of slot 0
	decision = selector.Feed(6000);
	EXPECT_TRUE(decision.hold);
	EXPECT_EQ(decision.decode_first, k4);
}

TEST(KeyframeSelector, SlotBoundaryIsTheFpsFilters)
{
	Selector selector({0.1});
	selector.Feed(0);

	// Rounded half away from zero: 4999 ms is slot 0, 5000 ms slot 1
	selector.Feed(4000);
	auto k4999 = Key(4999);
	EXPECT_EQ(selector.selector.OnKeyframe(k4999, true).decode_first, nullptr);
	EXPECT_EQ(selector.Feed(5000).decode_first, k4999);
}

TEST(KeyframeSelector, AKeyframeNeededByAnyRateIsDecoded)
{
	// 0.1 fps puts 2000 and 4000 in one slot, 0.5 fps (2 s slots) does not
	Selector selector({0.1, 0.5});
	selector.Feed(0);

	auto k2 = Key(2000);
	selector.selector.OnKeyframe(k2, true);
	EXPECT_EQ(selector.Feed(4000).decode_first, k2);
}

TEST(KeyframeSelector, OutputsOfTheSameRateDecideAsOne)
{
	Selector one({0.1});
	Selector three({0.1, 0.1, 0.1});

	for (int64_t pts : {0, 2000, 4000, 6000, 9000, 12000, 31000})
	{
		auto a = one.FeedAndProduce(pts);
		auto b = three.FeedAndProduce(pts);
		EXPECT_EQ(a.hold, b.hold) << pts;
		EXPECT_EQ(a.decode_first == nullptr, b.decode_first == nullptr) << pts;
	}
}

TEST(KeyframeSelector, AKeyframeThatSkipsASlotIsDecodedAtOnce)
{
	// 4000 is slot 0 and 16000 slot 2 at 0.1 fps: the filter fills slot 1 from the frames around 16000
	Selector selector({0.1});
	selector.Feed(0);

	auto k4 = Key(4000);
	selector.selector.OnKeyframe(k4, true);
	auto decision = selector.FeedAndProduce(16000);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, k4);

	// The next slot is not skipped
	EXPECT_TRUE(selector.Feed(24000).hold);
}

TEST(KeyframeSelector, AStepBackRestartsSelection)
{
	Selector selector({0.1});
	selector.Feed(0);
	ASSERT_TRUE(selector.Feed(6000).hold);

	// The held keyframe is dropped; the stamped-back one is decoded at once, as the first of a new run
	auto decision = selector.Feed(2000);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);

	// Holding resumes with the next one, counted from the keyframe that restarted
	EXPECT_TRUE(selector.Feed(3000).hold);
	auto after = selector.Feed(4000);
	EXPECT_TRUE(after.hold);
	EXPECT_EQ(after.decode_first, nullptr);
}

TEST(KeyframeSelector, AJumpOverTheLimitRestartsSelection)
{
	// 0.02 fps: 50 s slots, so only the jump separates the keyframes
	Selector selector({0.02});
	selector.Feed(0);
	ASSERT_TRUE(selector.Feed(2000).hold);

	auto decision = selector.Feed(17001);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);
	EXPECT_TRUE(selector.Feed(19000).hold);
}

TEST(KeyframeSelector, AJumpWithinTheLimitIsHeld)
{
	Selector selector({0.02});
	selector.Feed(0);

	EXPECT_TRUE(selector.Feed(15000).hold);
}

TEST(KeyframeSelector, AnOutputWithoutAnImageRestartsAfterTwoBoundaries)
{
	Selector selector({0.1});

	// Slots 0, 1, 2: two boundaries crossed, no image yet
	selector.Feed(0);
	ASSERT_TRUE(selector.Feed(10000).hold);
	ASSERT_TRUE(selector.Feed(20000).hold);

	// The held keyframe is dropped and this one is decoded at once
	auto decision = selector.Feed(30000);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);

	// A new run: the next keyframe is held again
	EXPECT_TRUE(selector.Feed(40000).hold);
}

TEST(KeyframeSelector, AnImageEachSlotKeepsSelectionGoing)
{
	Selector selector({0.1});
	selector.Feed(0);

	for (int64_t slot = 1; slot < 20; slot++)
	{
		EXPECT_TRUE(selector.FeedAndProduce(slot * 10000).hold) << slot;
	}
}

TEST(KeyframeSelector, ALongKeyframeIntervalWithImagesIsNotAStall)
{
	// 0.5 fps: a slot every 2 s, a keyframe every 4 slots, each followed by its images
	Selector selector({0.5});
	selector.Feed(0);
	for (int64_t i = 1; i <= 5; i++)
	{
		EXPECT_FALSE(selector.FeedAndProduce(i * 8000).hold);
	}

	// A keyframe in the next slot is held, and the one that skips a slot decodes it: nothing was restarted
	auto k42 = Key(42000);
	EXPECT_TRUE(selector.selector.OnKeyframe(k42, true).hold);
	EXPECT_EQ(selector.Feed(50000).decode_first, k42);
}

TEST(KeyframeSelector, ALongKeyframeIntervalWithoutImagesIsAStall)
{
	Selector selector({0.5});
	selector.Feed(0);
	EXPECT_FALSE(selector.Feed(8000).hold);

	// Four boundaries crossed, no image: restart, so the next keyframe, in the same slot, is decoded as well
	EXPECT_FALSE(selector.Feed(9000).hold);
	EXPECT_TRUE(selector.Feed(10000).hold);
}

TEST(KeyframeSelector, OneStalledOutputRestartsTheStream)
{
	// 0.5 fps: a slot every 2 s. Only the 0.1 fps output produces images
	Selector selector({0.1, 0.5});
	selector.Feed(0);
	for (int64_t pts : {2000, 4000})
	{
		selector.Feed(pts);
		selector.images[0]++;
	}

	EXPECT_FALSE(selector.Feed(6000).hold);
	EXPECT_TRUE(selector.Feed(8000).hold);
}

TEST(KeyframeSelector, AStalledOutputIsCappedWhileAnotherProducesImages)
{
	// 0.5 fps: a slot every 2 s, a keyframe every 2 s. Only the 0.1 fps output produces images
	Selector selector({0.1, 0.5});

	int decoded_at_once = 0;
	for (int64_t pts = 0; selector.selector.IsEnabled() && pts < 200000; pts += 2000)
	{
		decoded_at_once += selector.Feed(pts).hold ? 0 : 1;
		selector.images[0]++;
	}

	// The first keyframe, kMaxRestarts restarts, and the stall that gives up
	EXPECT_FALSE(selector.selector.IsEnabled());
	EXPECT_EQ(decoded_at_once, 1 + KeyframeSelector::kMaxRestarts + 1);
}

TEST(KeyframeSelector, AStalledSlowOutputIsCappedWhileRestartsForAFastOneComeAndGo)
{
	// A keyframe every 2 s. The 0.1 fps output never produces an image. The 0.5 fps one produces one after each
	// keyframe that starts a run, then stops again, so it keeps causing restarts of its own
	Selector selector({0.1, 0.5});

	int64_t pts = 0;
	for (; selector.selector.IsEnabled() && pts < 600000; pts += 2000)
	{
		if (selector.Feed(pts).hold == false)
		{
			selector.images[1]++;
		}
	}

	EXPECT_FALSE(selector.selector.IsEnabled());
}

TEST(KeyframeSelector, RestartsAreCapped)
{
	Selector selector({0.1});
	ASSERT_TRUE(selector.selector.IsEnabled());

	// Never an image. A keyframe in each slot: held twice, then the restart
	int64_t pts	 = 0;
	int restarts = 0;
	for (int i = 0; i < 40 && selector.selector.IsEnabled(); i++)
	{
		restarts += selector.Feed(pts).hold ? 0 : 1;
		pts += 10000;
	}

	// The first keyframe, kMaxRestarts restarts, and the stall that gives up
	EXPECT_FALSE(selector.selector.IsEnabled());
	EXPECT_EQ(restarts, 1 + KeyframeSelector::kMaxRestarts + 1);
}

TEST(KeyframeSelector, AnImageResetsTheRestartCount)
{
	Selector selector({0.1});
	int64_t pts = 0;

	// The first keyframe and two restarts
	int decoded_at_once = 0;
	for (int i = 0; i < 40 && decoded_at_once < 1 + 2; i++)
	{
		decoded_at_once += selector.Feed(pts).hold ? 0 : 1;
		pts += 10000;
	}
	ASSERT_EQ(decoded_at_once, 1 + 2);

	// An image, then the full allowance again
	selector.images[0]++;
	decoded_at_once = 0;
	for (int i = 0; i < 40 && selector.selector.IsEnabled(); i++)
	{
		decoded_at_once += selector.Feed(pts).hold ? 0 : 1;
		pts += 10000;
	}
	EXPECT_FALSE(selector.selector.IsEnabled());
	EXPECT_EQ(decoded_at_once, KeyframeSelector::kMaxRestarts + 1);
}

TEST(KeyframeSelector, AHeldKeyframeThatGivesNoPictureRestartsSelection)
{
	Selector selector({0.1});
	selector.Feed(0);
	ASSERT_TRUE(selector.FeedAndProduce(2000).hold);

	// 2000 is the last of its slot; its decode gives no picture, though the stand-in of 12000 keeps the images coming
	auto decision = selector.FeedAndProduce(12000);
	ASSERT_NE(decision.decode_first, nullptr);
	ASSERT_TRUE(decision.hold);
	selector.selector.OnHeldKeyframeDecoded(false);

	// 12000 is dropped, and the next keyframe is decoded as the first of a new run
	decision = selector.FeedAndProduce(13000);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);
	EXPECT_TRUE(selector.FeedAndProduce(14000).hold);
}

TEST(KeyframeSelector, AHeldKeyframeThatGivesAPictureDoesNotRestart)
{
	Selector selector({0.1});
	selector.Feed(0);
	ASSERT_TRUE(selector.FeedAndProduce(2000).hold);

	ASSERT_NE(selector.FeedAndProduce(12000).decode_first, nullptr);
	selector.selector.OnHeldKeyframeDecoded(true);

	EXPECT_TRUE(selector.FeedAndProduce(13000).hold);
}

namespace
{
	// A keyframe every 6 s, images coming out all the time. report: whether the decode of a held keyframe gives
	// a picture. Returns how many held keyframes were decoded before selection ended (at most max_decodes)
	int FeedUntilDisabled(Selector &selector, const std::function<bool(int)> &report, int max_decodes)
	{
		int decodes = 0;
		for (int64_t pts = 0; selector.selector.IsEnabled() && decodes < max_decodes; pts += 6000)
		{
			auto decision = selector.FeedAndProduce(pts);
			if (decision.decode_first != nullptr && selector.selector.IsEnabled())
			{
				selector.selector.OnHeldKeyframeDecoded(report(decodes++));
			}
		}
		return decodes;
	}
}  // namespace

TEST(KeyframeSelector, RepeatedDecodesWithoutAPictureAreCappedWhileImagesComeOut)
{
	Selector selector({0.1});

	// The stand-ins keep the images coming; the decodes never give a picture. Three restarts, then the fourth failed decode ends selection
	int decodes = FeedUntilDisabled(selector, [](int) { return false; }, 100);
	EXPECT_FALSE(selector.selector.IsEnabled());
	EXPECT_EQ(decodes, KeyframeSelector::kMaxRestarts + 1);
}

TEST(KeyframeSelector, ADecodeWithAPictureResetsTheFailedDecodeCount)
{
	Selector selector({0.1});

	// Fail, fail, succeed, then every other decode fails: the count never reaches the cap
	int decodes = FeedUntilDisabled(selector, [](int n) { return n >= 2 && n % 2 == 0; }, 30);
	EXPECT_TRUE(selector.selector.IsEnabled());
	EXPECT_EQ(decodes, 30);
}

TEST(KeyframeSelector, ADisabledSelectorStaysDisabled)
{
	Selector selector({0.1});
	selector.Feed(0);
	ASSERT_TRUE(selector.Feed(2000).hold);

	selector.selector.Disable();
	EXPECT_FALSE(selector.selector.IsEnabled());

	// A new codec session does not bring it back
	selector.Reconfigure();
	EXPECT_FALSE(selector.selector.IsEnabled());
}

TEST(KeyframeSelector, ConfigureStartsANewRun)
{
	Selector selector({0.1});
	selector.Feed(0);
	ASSERT_TRUE(selector.Feed(2000).hold);

	selector.Reconfigure();

	// The first keyframe of a run is decoded, and the held one is gone
	auto decision = selector.Feed(4000);
	EXPECT_FALSE(decision.hold);
	EXPECT_EQ(decision.decode_first, nullptr);
}

TEST(KeyframeSelector, SupportedCodecs)
{
	EXPECT_TRUE(KeyframeSelector::IsSupportedCodec(cmn::MediaCodecId::H264));
	EXPECT_TRUE(KeyframeSelector::IsSupportedCodec(cmn::MediaCodecId::Vp8));
	EXPECT_FALSE(KeyframeSelector::IsSupportedCodec(cmn::MediaCodecId::H265));
	EXPECT_FALSE(KeyframeSelector::IsSupportedCodec(cmn::MediaCodecId::Av1));
}

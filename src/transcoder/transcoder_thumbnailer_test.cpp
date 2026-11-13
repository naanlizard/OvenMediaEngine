//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <base/info/stream.h>
#include <base/ovsocket/socket_test_helpers.h>
#include <modules/bitstream/h264/h264_decoder_configuration_record.h>
#include <modules/bitstream/h264/h264_parser.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "transcoder_thumbnailer.h"

namespace
{
	// One keyframe each with its parameter sets, by libx264 and libvpx: a 64x48 gray picture from FFmpeg's color
	// source (128x72 in kH264Wide, whose SPS has the id of the one kH264Dark and kH264Bright share)
	const std::vector<uint8_t> kH264Dark = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xd9, 0x04, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03,
		0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc0, 0xf1, 0x22, 0x64, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68,
		0xcb, 0x83, 0xcb, 0x20, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x09, 0xf2, 0x62, 0x80, 0x00, 0x99,
		0x1c, 0x9c, 0x9c, 0x9d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5e};

	const std::vector<uint8_t> kH264Bright = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xd9, 0x04, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03,
		0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc0, 0xf1, 0x22, 0x64, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68,
		0xcb, 0x83, 0xcb, 0x20, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x09, 0xf2, 0x62, 0x80, 0x00, 0x97,
		0x94, 0x9c, 0x9c, 0x9d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5e};

	const std::vector<uint8_t> kH264Wide = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xd9, 0x02, 0x0b, 0xf9, 0x70, 0x11, 0x00, 0x00,
		0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x3c, 0x0f, 0x12, 0x26, 0x48, 0x00, 0x00, 0x00, 0x01,
		0x68, 0xcb, 0x83, 0xcb, 0x20, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x09, 0xf2, 0x62, 0x80, 0x01,
		0x39, 0x39, 0x39, 0x39, 0x39, 0x39, 0x39, 0x3a, 0xeb, 0xae, 0xba, 0xeb, 0xae, 0xba, 0xeb, 0xae,
		0xba, 0xeb, 0xae, 0xba, 0xeb, 0xae, 0xba, 0xeb, 0xae, 0xba, 0xeb, 0xae, 0xba, 0xeb, 0xae, 0xbc};

	// Its SPS declares two reordered frames (x264 with -bf 2)
	const std::vector<uint8_t> kH264Reorder = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x0a, 0xac, 0xd9, 0x44, 0x7b, 0x01, 0x10, 0x00, 0x00,
		0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc0, 0xf1, 0x22, 0x59, 0x60, 0x00, 0x00, 0x00, 0x01,
		0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x27, 0xff, 0xfe,
		0xf5, 0xb1, 0x7c, 0x0a, 0x68, 0x19, 0xb6, 0x28, 0x33, 0xa0, 0xc8, 0xb7, 0x29, 0x6c, 0xc8, 0x49,
		0x8d, 0x56, 0xbd, 0xbb, 0x19};

	const std::vector<uint8_t> kVp8 = {
		0xb0, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x40, 0x00, 0x30, 0x00, 0x00, 0x47, 0x08, 0x85, 0x85, 0x88,
		0x85, 0x84, 0x88, 0x02, 0x02, 0x00, 0x06, 0x8e, 0x62, 0x13, 0x67, 0x99, 0x8f, 0x2f, 0x00, 0xfe,
		0xff, 0x03, 0xe8, 0x00};

	// kH264Bright cut off inside the slice header
	const std::vector<uint8_t> kH264Truncated(kH264Bright.begin(), kH264Bright.begin() + 42);

	// SPS and PPS of x264 with tff=1: frame_mbs_only_flag is 0
	const std::vector<uint8_t> kH264FieldsAllowed = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x15, 0xf4, 0x22, 0x7e, 0x5c, 0x04, 0x40, 0x00, 0x00,
		0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x87, 0xc5, 0x0a, 0xa8, 0x00, 0x00, 0x00, 0x01, 0x68, 0xde,
		0x0f, 0xc8};

	// SPS and PPS of x264 with open-gop=1, bframes=2, keyint=4 (a 64x48 light gray picture)
	const std::vector<uint8_t> kH264OpenGopParameterSets = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x0a, 0xec, 0xa2, 0x3d, 0x80, 0x88, 0x00, 0x00, 0x03,
		0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0xe0, 0x78, 0x91, 0x2c, 0xb0, 0x00, 0x00, 0x00, 0x01, 0x68,
		0xce, 0x0f, 0xc8};

	// Its second keyframe as an avcC stream carries it: a recovery point SEI and a non-IDR I slice
	const std::vector<uint8_t> kH264OpenGopKeyframe = {
		0x00, 0x00, 0x00, 0x01, 0x06, 0x06, 0x01, 0xc4, 0x80, 0x00, 0x00, 0x00, 0x01, 0x41, 0x88, 0x99,
		0x00, 0xea, 0x26, 0x28, 0x00, 0x0a, 0xca, 0x49, 0xc9, 0xc9, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
		0xe0};

	// SPS of kH264OpenGopParameterSets and a PPS with id 1 (a copy of its PPS 0), then kH264OpenGopKeyframe, which
	// refers to PPS 0
	const std::vector<uint8_t> kH264OpenGopKeyframeWithOtherPps = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x0a, 0xec, 0xa2, 0x3d, 0x80, 0x88, 0x00, 0x00, 0x03,
		0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0xe0, 0x78, 0x91, 0x2c, 0xb0, 0x00, 0x00, 0x00, 0x01, 0x68,
		0x53, 0x83, 0xf2, 0x00, 0x00, 0x00, 0x01, 0x06, 0x06, 0x01, 0xc4, 0x80, 0x00, 0x00, 0x00, 0x01,
		0x41, 0x88, 0x99, 0x00, 0xea, 0x26, 0x28, 0x00, 0x0a, 0xca, 0x49, 0xc9, 0xc9, 0xd7, 0x5d, 0x75,
		0xd7, 0x5d, 0x75, 0xe0};

	// A copy of the PPS of kH264OpenGopParameterSets with id 1
	const std::vector<uint8_t> kH264OpenGopPps1 = {
		0x00, 0x00, 0x00, 0x01, 0x68, 0x53, 0x83, 0xf2};

	// kH264OpenGopKeyframe with its slice referring to PPS 1
	const std::vector<uint8_t> kH264OpenGopKeyframeOnPps1 = {
		0x00, 0x00, 0x00, 0x01, 0x06, 0x06, 0x01, 0xc4, 0x80, 0x00, 0x00, 0x00, 0x01, 0x41, 0x88, 0x46,
		0x40, 0x3a, 0x89, 0x8a, 0x00, 0x02, 0xb2, 0x92, 0x72, 0x72, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
		0x78};

	// kH264OpenGopKeyframe without its recovery point SEI
	const std::vector<uint8_t> kH264OpenGopKeyframeWithoutSei(kH264OpenGopKeyframe.begin() + 9, kH264OpenGopKeyframe.end());

	// Two PPS with id 1 that the decoder rejects: one cut off after its id, one that names SPS 5
	const std::vector<uint8_t> kH264Pps1Truncated	 = {0x00, 0x00, 0x00, 0x01, 0x68, 0x40};
	const std::vector<uint8_t> kH264Pps1OnMissingSps = {0x00, 0x00, 0x00, 0x01, 0x68, 0x46, 0x80};

	// kH264OpenGopParameterSets' PPS with entropy_coding_mode_flag set: wrong for kH264OpenGopKeyframe
	const std::vector<uint8_t> kH264OpenGopPps0Cabac = {0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x0f, 0xc8};

	// kH264OpenGopParameterSets with num_ref_idx_l0_default_active_minus1 = 2 in the PPS, as x264 writes by default.
	// FFmpeg takes a keyframe without recovery point for a random access point only up to two reference pictures.
	const std::vector<uint8_t> kH264OpenGopParameterSetsThreeReferences = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x0a, 0xec, 0xa2, 0x3d, 0x80, 0x88, 0x00, 0x00, 0x03,
		0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0xe0, 0x78, 0x91, 0x2c, 0xb0, 0x00, 0x00, 0x00, 0x01, 0x68,
		0xcb, 0x83, 0xf2};

	// The recovery point SEI of kH264OpenGopKeyframe: recovery_frame_cnt 0
	const std::vector<uint8_t> kH264RecoveryPointSei(kH264OpenGopKeyframe.begin(), kH264OpenGopKeyframe.begin() + 9);

	// The first three pictures of x264 with bframes=0 for a light gray, then dark gray 64x48 picture: parameter
	// sets and an IDR slice; an I slice that is not an IDR (a scene cut); a P slice
	const std::vector<uint8_t> kH264Idr = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xda, 0x11, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03,
		0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0x83, 0xc4, 0x89, 0xa8, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce,
		0x0f, 0xc8, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x05, 0x7f, 0xff, 0xff, 0x0f, 0x45, 0x00,
		0x01, 0x27, 0x2f, 0x27, 0x27, 0x27, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x80};
	const std::vector<uint8_t> kH264NonIdr = {
		0x00, 0x00, 0x00, 0x01, 0x41, 0x88, 0x88, 0x17, 0xff, 0xff, 0xfc, 0x11, 0x45, 0x00, 0x01, 0x34,
		0x5f, 0x1c, 0x00, 0x63, 0x80, 0x0c, 0x9c, 0x9c, 0x9d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5e};
	const std::vector<uint8_t> kH264PSlice = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x40, 0x16, 0xf1, 0xb0};

	constexpr MediaTrackId kDecoderId = 7;

	std::vector<uint8_t> Join(std::initializer_list<std::vector<uint8_t>> parts)
	{
		std::vector<uint8_t> joined;
		for (auto &part : parts)
		{
			joined.insert(joined.end(), part.begin(), part.end());
		}

		return joined;
	}

	// A keyframe as a publisher sends it when it does not repeat the parameter sets
	std::vector<uint8_t> Slices(const std::vector<uint8_t> &data)
	{
		std::vector<uint8_t> slices;
		for (const auto &index : H264Parser::FindNaluIndexes(data.data(), data.size()))
		{
			auto type = data[index._payload_offset] & 0x1F;
			if ((type == 1) || (type == 5))
			{
				slices.insert(slices.end(), {0x00, 0x00, 0x00, 0x01});
				slices.insert(slices.end(), data.begin() + index._payload_offset, data.begin() + index._payload_offset + index._payload_size);
			}
		}

		return slices;
	}

	// The avcC a publisher would announce for these bytes
	std::shared_ptr<AVCDecoderConfigurationRecord> MakeAvcRecord(const std::vector<uint8_t> &data)
	{
		auto record = std::make_shared<AVCDecoderConfigurationRecord>();
		for (const auto &index : H264Parser::FindNaluIndexes(data.data(), data.size()))
		{
			auto nalu = std::make_shared<ov::Data>(data.data() + index._payload_offset, index._payload_size);
			switch (data[index._payload_offset] & 0x1F)
			{
				case 7:
					record->AddSPS(nalu);
					break;
				case 8:
					record->AddPPS(nalu);
					break;
			}
		}

		return record;
	}

	std::shared_ptr<MediaTrack> MakeInputTrack(cmn::MediaCodecId codec_id = cmn::MediaCodecId::H264)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(0);
		track->SetMediaType(cmn::MediaType::Video);
		track->SetCodecId(codec_id);
		track->SetTimeBase(1, 1000);
		track->SetKeyframeDecodeOnly(true);

		return track;
	}

	std::shared_ptr<MediaTrack> MakeOutputTrack(MediaTrackId id, double framerate)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(id);
		track->SetMediaType(cmn::MediaType::Video);
		track->SetCodecId(cmn::MediaCodecId::Jpeg);
		track->SetTimeBase(1, 90000);
		track->SetFrameRateByConfig(framerate);
		track->SetResolution(32, 24);
		track->SetColorspace(cmn::VideoPixelFormatId::YUVJ420P);

		return track;
	}

	std::shared_ptr<info::Stream> MakeStream()
	{
		return std::make_shared<info::Stream>(StreamSourceType::Ovt);
	}

	struct Result
	{
		TranscodeResult result;
		std::shared_ptr<MediaFrame> frame;
		// [FILTER_ID, SCALED FRAME]
		std::map<MediaTrackId, std::shared_ptr<MediaFrame>> scaled;
	};

	// [PTS, FILTER_IDS] of an image
	typedef std::pair<int64_t, std::vector<MediaTrackId>> Image;

	class Thumbnails
	{
	public:
		// The id of an output track is also its filter id
		explicit Thumbnails(std::vector<std::shared_ptr<MediaTrack>> output_tracks = {MakeOutputTrack(100, 1.0)}, std::shared_ptr<MediaTrack> input_track = MakeInputTrack())
		{
			auto input_stream = MakeStream();

			TranscodeThumbnailer::Outputs outputs;
			for (auto &output_track : output_tracks)
			{
				outputs.emplace_back(input_stream, input_track, MakeStream(), output_track, output_track->GetId());
			}

			_thumbnailer = TranscodeThumbnailer::Create(
				kDecoderId, input_track, outputs,
				[this, input_track](TranscodeResult result, MediaTrackId decoder_id, std::shared_ptr<MediaFrame> frame) {
					if (result == TranscodeResult::FormatChanged)
					{
						// What TranscoderStream::UpdateInputTrack() does with it
						input_track->SetResolution(frame->GetWidth(), frame->GetHeight());
						input_track->SetColorspace(frame->GetFormat<cmn::VideoPixelFormatId>());
						input_track->SetColorMatrix(frame->GetColorMatrix());
						input_track->SetColorRange(frame->GetColorRange());
					}

					std::unique_lock<std::mutex> lock(_mutex);
					_results.push_back({result, std::move(frame), {}});
					_condition.wait(lock, [this]() { return _hold == false; });
				},
				[this](TranscodeResult result, MediaTrackId filter_id, std::shared_ptr<MediaFrame> frame) {
					std::lock_guard<std::mutex> lock(_mutex);
					ASSERT_FALSE(_results.empty()) << "scaled frame before its picture";
					EXPECT_TRUE(_results.back().scaled.emplace(filter_id, std::move(frame)).second) << "second frame for filter " << filter_id;
				});
			EXPECT_NE(_thumbnailer, nullptr);
		}

		static std::shared_ptr<MediaPacket> MakePacket(const std::vector<uint8_t> &data, int64_t pts, MediaPacketFlag flag)
		{
			// The decoders do not read the format of a key packet
			return std::make_shared<MediaPacket>(
				cmn::MediaType::Video, 0, data.data(), data.size(), pts, pts, 0,
				flag, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
		}

		std::shared_ptr<MediaPacket> Send(const std::vector<uint8_t> &data, int64_t pts, MediaPacketFlag flag = MediaPacketFlag::Key)
		{
			auto packet = MakePacket(data, pts, flag);
			if (_thumbnailer != nullptr)
			{
				_thumbnailer->SendBuffer(packet);
			}

			return packet;
		}

		// Returns once the worker has taken the packet and let it go, with or without decoding its picture
		void SendAndWait(const std::vector<uint8_t> &data, int64_t pts)
		{
			auto packet = Send(data, pts);
			EXPECT_TRUE(ome_test::WaitUntil([&]() { return packet.use_count() == 1; }, 10000));
		}

		void Hold(bool hold)
		{
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_hold = hold;
			}
			_condition.notify_all();
		}

		void Stop()
		{
			_thumbnailer.reset();
		}

		// What a packet that is not a keyframe tells: it is dropped, or refused with every other packet
		bool Refused()
		{
			return _thumbnailer->SendBuffer(MakePacket(kH264PSlice, 0, MediaPacketFlag::NoFlag)) == false;
		}

		// Returns while the worker is in the report of this keyframe's picture, where it stays until Hold(false)
		void SendAndHold(const std::vector<uint8_t> &data, int64_t pts)
		{
			auto reports = Results().size() + 1;
			Hold(true);
			Send(data, pts);
			ASSERT_TRUE(ome_test::WaitUntil([&]() { return Results().size() == reports; }, 10000));
		}

		std::vector<Result> Results()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _results;
		}

		std::vector<Image> Images()
		{
			std::vector<Image> images;
			for (auto &result : Results())
			{
				if (result.frame == nullptr)
				{
					continue;
				}

				images.emplace_back(result.frame->GetPts(), std::vector<MediaTrackId>{});
				for (auto &[filter_id, scaled] : result.scaled)
				{
					if (scaled != nullptr)
					{
						images.back().second.push_back(filter_id);
					}
				}
			}

			return images;
		}

	private:
		std::mutex _mutex;
		std::condition_variable _condition;
		std::vector<Result> _results;
		bool _hold = false;

		std::shared_ptr<TranscodeThumbnailer> _thumbnailer;
	};

	uint8_t Luma(const std::shared_ptr<MediaFrame> &frame)
	{
		return frame->GetData()->GetPlaneData(0)[0];
	}
}  // namespace

TEST(TranscodeThumbnailer, IsSupportedOnlyForKeyframeOnlyTracksWithoutSkipFrames)
{
	auto input_track = MakeInputTrack();
	input_track->SetDecoderConfigurationRecord(MakeAvcRecord(kH264Dark));
	auto skipping = MakeOutputTrack(101, 1.0);
	TranscodeThumbnailer::Outputs outputs{
		{MakeStream(), input_track, MakeStream(), MakeOutputTrack(100, 1.0), 100},
		{MakeStream(), input_track, MakeStream(), skipping, 101}};

	EXPECT_TRUE(TranscodeThumbnailer::IsSupported(input_track, outputs));

	skipping->SetSkipFramesByConfig(0);
	EXPECT_FALSE(TranscodeThumbnailer::IsSupported(input_track, outputs));

	skipping->SetSkipFramesByConfig(-1);
	input_track->SetKeyframeDecodeOnly(false);
	EXPECT_FALSE(TranscodeThumbnailer::IsSupported(input_track, outputs));
}

TEST(TranscodeThumbnailer, IsSupportedForVp8AndForH264WhenEverySpsRulesOutFields)
{
	auto input_track = MakeInputTrack();
	TranscodeThumbnailer::Outputs outputs{{MakeStream(), input_track, MakeStream(), MakeOutputTrack(100, 1.0), 100}};

	EXPECT_FALSE(TranscodeThumbnailer::IsSupported(input_track, outputs));

	input_track->SetDecoderConfigurationRecord(MakeAvcRecord(kH264Dark));
	EXPECT_TRUE(TranscodeThumbnailer::IsSupported(input_track, outputs));

	input_track->SetDecoderConfigurationRecord(MakeAvcRecord(kH264FieldsAllowed));
	EXPECT_FALSE(TranscodeThumbnailer::IsSupported(input_track, outputs));

	EXPECT_TRUE(TranscodeThumbnailer::IsSupported(MakeInputTrack(cmn::MediaCodecId::Vp8), outputs));

	// Their decoders do not keep the parameter sets of a packet they skip, so the pipeline decodes these
	EXPECT_FALSE(TranscodeThumbnailer::IsSupported(MakeInputTrack(cmn::MediaCodecId::H265), outputs));
	EXPECT_FALSE(TranscodeThumbnailer::IsSupported(MakeInputTrack(cmn::MediaCodecId::Av1), outputs));
}

TEST(TranscodeThumbnailer, IsNotCreatedForACodecItCannotDecode)
{
	EXPECT_EQ(TranscodeThumbnailer::Create(0, MakeInputTrack(cmn::MediaCodecId::H265), {}, nullptr, nullptr), nullptr);
}

TEST(TranscodeThumbnailer, SlotIsTheNearestImageTime)
{
	cmn::Timebase milliseconds(1, 1000);

	EXPECT_EQ(TranscodeThumbnailer::GetSlot(0, milliseconds, 1.0), 0);
	EXPECT_EQ(TranscodeThumbnailer::GetSlot(499, milliseconds, 1.0), 0);
	EXPECT_EQ(TranscodeThumbnailer::GetSlot(500, milliseconds, 1.0), 1);
	EXPECT_EQ(TranscodeThumbnailer::GetSlot(1499, milliseconds, 1.0), 1);
	EXPECT_EQ(TranscodeThumbnailer::GetSlot(1500, milliseconds, 1.0), 2);
	EXPECT_EQ(TranscodeThumbnailer::GetSlot(-500, milliseconds, 1.0), -1);

	EXPECT_EQ(TranscodeThumbnailer::GetSlot(999, milliseconds, 0.5), 0);
	EXPECT_EQ(TranscodeThumbnailer::GetSlot(1000, milliseconds, 0.5), 1);

	EXPECT_EQ(TranscodeThumbnailer::GetSlot(3000 * 7, cmn::Timebase(1, 90000), 30.0), 7);

	EXPECT_EQ(TranscodeThumbnailer::GetSlot(123456, milliseconds, 0.0), 0);
}

TEST(TranscodeThumbnailer, FirstKeyframeOfEachSlotGivesOneImage)
{
	Thumbnails thumbnails;

	thumbnails.SendAndWait(kH264Dark, 0);
	thumbnails.SendAndWait(kH264Bright, 200);
	thumbnails.SendAndWait(kH264Bright, 400);
	thumbnails.SendAndWait(kH264Bright, 600);
	thumbnails.SendAndWait(kH264Dark, 900);
	thumbnails.SendAndWait(kH264Dark, 1600);
	// When time steps back there is no image until it has passed the last one, as on the pipeline
	thumbnails.SendAndWait(kH264Dark, 700);
	thumbnails.SendAndWait(kH264Dark, 1400);
	thumbnails.SendAndWait(kH264Bright, 2500);
	thumbnails.Stop();

	ASSERT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {600, {100}}, {1600, {100}}, {2500, {100}}}));

	auto results = thumbnails.Results();
	ASSERT_EQ(results.size(), 4u);
	EXPECT_LT(Luma(results[0].scaled[100]), 100);
	EXPECT_GT(Luma(results[1].scaled[100]), 150);
	EXPECT_LT(Luma(results[2].scaled[100]), 100);
	EXPECT_GT(Luma(results[3].scaled[100]), 150);
}

TEST(TranscodeThumbnailer, PacketsThatAreNotKeyframesAreDropped)
{
	Thumbnails thumbnails;

	thumbnails.SendAndWait(Join({kH264OpenGopParameterSets, kH264OpenGopKeyframe}), 0);

	// Neither its picture nor its PPS, which the keyframes do not decode with, is used
	auto other = thumbnails.Send(Join({kH264OpenGopPps0Cabac, kH264OpenGopKeyframe}), 1000, MediaPacketFlag::NoFlag);
	EXPECT_EQ(other.use_count(), 1);
	auto empty = thumbnails.Send({}, 1100);
	EXPECT_EQ(empty.use_count(), 1);

	thumbnails.SendAndWait(kH264OpenGopKeyframe, 1200);
	thumbnails.Stop();

	ASSERT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1200, {100}}}));

	auto results = thumbnails.Results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_GT(Luma(results[1].frame), 150);
}

TEST(TranscodeThumbnailer, EachOutputFollowsItsOwnRate)
{
	Thumbnails thumbnails({MakeOutputTrack(100, 1.0), MakeOutputTrack(101, 0.5)});

	thumbnails.SendAndWait(kH264Dark, 0);
	thumbnails.SendAndWait(kH264Dark, 600);
	thumbnails.SendAndWait(kH264Dark, 1200);
	thumbnails.SendAndWait(kH264Dark, 1300);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100, 101}}, {600, {100}}, {1200, {101}}}));
}

TEST(TranscodeThumbnailer, OutputsThatShareAFilterGiveOneImage)
{
	Thumbnails thumbnails({MakeOutputTrack(100, 1.0), MakeOutputTrack(100, 1.0)});

	thumbnails.SendAndWait(kH264Dark, 0);
	thumbnails.SendAndWait(kH264Dark, 1000);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1000, {100}}}));
}

TEST(TranscodeThumbnailer, WaitingKeyframesAreDecodedInOrder)
{
	Thumbnails thumbnails;

	thumbnails.SendAndHold(kH264Dark, 0);

	// The first needs the SPS that came before it; the second brings another under the same id
	thumbnails.Send(Slices(kH264Bright), 1000);
	auto last = thumbnails.Send(kH264Wide, 2000);

	thumbnails.Hold(false);
	EXPECT_TRUE(ome_test::WaitUntil([&]() { return last.use_count() == 1; }, 10000));
	thumbnails.Stop();

	ASSERT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1000, {100}}, {2000, {100}}}));

	auto results = thumbnails.Results();
	ASSERT_EQ(results.size(), 3u);
	EXPECT_EQ(results[1].frame->GetWidth(), 64);
	EXPECT_GT(Luma(results[1].frame), 150);
	EXPECT_EQ(results[2].frame->GetWidth(), 128);

	// The stream is told of a new format, which is the first one and each one after it
	EXPECT_EQ(results[0].result, TranscodeResult::FormatChanged);
	EXPECT_EQ(results[1].result, TranscodeResult::DataReady);
	EXPECT_EQ(results[2].result, TranscodeResult::FormatChanged);

	// Each picture is scaled from its own size
	for (auto &result : results)
	{
		auto scaled = result.scaled[100];
		ASSERT_NE(scaled, nullptr);
		EXPECT_EQ(scaled->GetWidth(), 32);
		EXPECT_EQ(scaled->GetHeight(), 24);
		EXPECT_EQ(scaled->GetPts(), result.frame->GetPts() * 90);
	}

	EXPECT_LT(Luma(results[0].scaled[100]), 100);
	EXPECT_GT(Luma(results[1].scaled[100]), 150);
}

TEST(TranscodeThumbnailer, KeyframeThatWaitedLongerThanAnImageIntervalIsDroppedForANewerOne)
{
	// The shortest interval counts: 100 ms
	Thumbnails thumbnails({MakeOutputTrack(100, 10.0), MakeOutputTrack(101, 1.0 / 60)});

	thumbnails.SendAndHold(kH264Dark, 0);

	auto stale = thumbnails.Send(kH264Bright, 1000);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	auto newer = thumbnails.Send(kH264Bright, 2000);

	thumbnails.Hold(false);
	EXPECT_TRUE(ome_test::WaitUntil([&]() { return newer.use_count() == 1; }, 10000));
	EXPECT_EQ(stale.use_count(), 1);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100, 101}}, {2000, {100}}}));
}

TEST(TranscodeThumbnailer, WaitingKeyframeIsDecodedWhenNoNewerOneWaitsOrItWaitedLessThanAnImageInterval)
{
	{
		Thumbnails thumbnails({MakeOutputTrack(100, 10.0)});

		thumbnails.SendAndHold(kH264Dark, 0);
		auto last = thumbnails.Send(kH264Bright, 1000);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		thumbnails.Hold(false);
		EXPECT_TRUE(ome_test::WaitUntil([&]() { return last.use_count() == 1; }, 10000));
		thumbnails.Stop();

		EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1000, {100}}}));
	}

	// An image a minute
	Thumbnails thumbnails({MakeOutputTrack(100, 1.0 / 60)});

	thumbnails.SendAndHold(kH264Dark, 0);
	thumbnails.Send(kH264Bright, 60000);
	auto last = thumbnails.Send(kH264Bright, 120000);

	thumbnails.Hold(false);
	EXPECT_TRUE(ome_test::WaitUntil([&]() { return last.use_count() == 1; }, 10000));
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {60000, {100}}, {120000, {100}}}));
}

TEST(TranscodeThumbnailer, RejectedKeyframeGivesNoImageAndKeepsTheSlotOpen)
{
	Thumbnails thumbnails;

	thumbnails.SendAndWait(kH264Dark, 0);
	thumbnails.SendAndWait(kH264Truncated, 1000);
	thumbnails.SendAndWait(kH264Bright, 1200);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1200, {100}}}));

	// A damaged keyframe is no error of the decoder, as on the pipeline
	EXPECT_EQ(thumbnails.Results().size(), 2u);
}

TEST(TranscodeThumbnailer, ScalerThatCouldNotBeSetUpGivesNoImageAndIsMadeAgain)
{
	auto output_track = MakeOutputTrack(100, 1.0);
	Thumbnails thumbnails({output_track});

	output_track->SetResolution(1000000, 1000000);
	thumbnails.SendAndWait(kH264Dark, 0);
	// The worker reads the output size when it scales, after it lets the packet go
	ASSERT_TRUE(ome_test::WaitUntil([&]() {
		auto results = thumbnails.Results();
		return (results.size() == 1) && (results[0].scaled.size() == 1);
	}, 10000));
	output_track->SetResolution(32, 24);
	thumbnails.SendAndWait(kH264Bright, 1000);
	thumbnails.Stop();

	ASSERT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {}}, {1000, {100}}}));

	auto results = thumbnails.Results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].scaled.count(100), 1u);
}

TEST(TranscodeThumbnailer, StopWaitsForTheImageInProgressAndDropsTheWaitingKeyframes)
{
	Thumbnails thumbnails;

	thumbnails.SendAndHold(kH264Dark, 0);

	thumbnails.Send(kH264Bright, 1000);
	thumbnails.Send(kH264Bright, 2000);

	std::atomic<bool> stopped{false};
	std::thread stopper([&]() {
		thumbnails.Stop();
		stopped = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_FALSE(stopped);

	thumbnails.Hold(false);
	stopper.join();

	EXPECT_EQ(thumbnails.Results().size(), 1u);
	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}}));
}

TEST(TranscodeThumbnailer, FirstKeyframeOfEachSlotGivesItsPictureForEachCodec)
{
	const std::vector<std::pair<cmn::MediaCodecId, const std::vector<uint8_t> *>> inputs = {
		{cmn::MediaCodecId::H264, &kH264Bright},
		{cmn::MediaCodecId::H264, &kH264Reorder},
		{cmn::MediaCodecId::Vp8, &kVp8}};

	for (auto &[codec_id, data] : inputs)
	{
		SCOPED_TRACE(ov::String::FormatString("%s, %zu bytes", cmn::GetCodecIdString(codec_id), data->size()).CStr());

		auto input_track = MakeInputTrack(codec_id);
		Thumbnails thumbnails({MakeOutputTrack(100, 1.0)}, input_track);
		EXPECT_EQ(input_track->GetCodecModuleId(), cmn::MediaCodecModuleId::DEFAULT);

		thumbnails.SendAndWait(*data, 3000);
		thumbnails.SendAndWait(*data, 3200);
		thumbnails.SendAndWait(*data, 4000);
		thumbnails.Stop();

		EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{3000, {100}}, {4000, {100}}}));

		auto results = thumbnails.Results();
		ASSERT_EQ(results.size(), 2u);
		ASSERT_NE(results[0].frame, nullptr);
		EXPECT_EQ(results[0].frame->GetTrackId(), kDecoderId);
		EXPECT_EQ(results[0].frame->GetPts(), 3000);
		EXPECT_EQ(results[0].frame->GetWidth(), 64);
		EXPECT_EQ(results[0].frame->GetHeight(), 48);
		EXPECT_GT(Luma(results[0].frame), 150);
	}
}

// The media router puts the parameter sets in front of IDR keyframes only, and a publisher may send a set once
TEST(TranscodeThumbnailer, ParameterSetsOfAnEarlierKeyframeAreKept)
{
	const auto open_gop = Join({kH264OpenGopParameterSets, kH264OpenGopKeyframe});
	const std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> inputs = {
		{kH264Dark, Slices(kH264Bright)},
		{kH264Reorder, Slices(kH264Reorder)},
		{open_gop, kH264OpenGopKeyframe},
		{open_gop, kH264OpenGopKeyframeWithOtherPps},
		{open_gop, kH264OpenGopKeyframeWithoutSei}};

	for (auto &[first, second] : inputs)
	{
		SCOPED_TRACE(ov::String::FormatString("%zu and %zu bytes", first.size(), second.size()).CStr());

		Thumbnails thumbnails;

		thumbnails.SendAndWait(first, 1000);
		thumbnails.SendAndWait(second, 2000);
		thumbnails.SendAndWait(second, 3000);
		thumbnails.Stop();

		ASSERT_EQ(thumbnails.Images(), (std::vector<Image>{{1000, {100}}, {2000, {100}}, {3000, {100}}}));

		auto results = thumbnails.Results();
		ASSERT_EQ(results.size(), 3u);
		EXPECT_GT(Luma(results[2].frame), 150);
	}
}

TEST(TranscodeThumbnailer, ParameterSetsOfAKeyframeNoImageNeedsAreKept)
{
	Thumbnails thumbnails;

	thumbnails.SendAndWait(kH264Dark, 0);
	thumbnails.SendAndWait(kH264Wide, 200);
	thumbnails.SendAndWait(Slices(kH264Wide), 1000);
	thumbnails.SendAndWait(Join({kH264OpenGopParameterSets, kH264OpenGopKeyframe}), 1200);
	thumbnails.SendAndWait(kH264OpenGopKeyframe, 2000);
	thumbnails.Stop();

	ASSERT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1000, {100}}, {2000, {100}}}));

	auto results = thumbnails.Results();
	ASSERT_EQ(results.size(), 3u);
	EXPECT_EQ(results[1].frame->GetWidth(), 128);
	EXPECT_EQ(results[2].frame->GetWidth(), 64);
	EXPECT_GT(Luma(results[2].frame), 150);
}

TEST(TranscodeThumbnailer, RejectedParameterSetsLeaveTheAcceptedOnesInEffect)
{
	auto with = [](const std::vector<uint8_t> &parameter_sets) {
		return Join({parameter_sets, kH264OpenGopKeyframeOnPps1});
	};

	Thumbnails thumbnails;

	thumbnails.SendAndWait(with(Join({kH264OpenGopParameterSets, kH264OpenGopPps1})), 1000);

	// Two rejected versions of PPS 1 in a row, in keyframes no image needs
	thumbnails.SendAndWait(with(kH264Pps1Truncated), 1100);
	thumbnails.SendAndWait(with(kH264Pps1OnMissingSps), 1200);
	thumbnails.SendAndWait(kH264OpenGopKeyframeOnPps1, 2000);

	// The same in keyframes that are decoded
	thumbnails.SendAndWait(with(kH264Pps1Truncated), 3000);
	thumbnails.SendAndWait(with(kH264Pps1OnMissingSps), 4000);
	thumbnails.SendAndWait(kH264OpenGopKeyframeOnPps1, 5000);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{1000, {100}}, {2000, {100}}, {3000, {100}}, {4000, {100}}, {5000, {100}}}));
}

// As the decoder of the pipeline, which is never flushed: it holds such keyframes back until it has given a picture
TEST(TranscodeThumbnailer, KeyframeWithoutRecoveryPointGivesAnImageOnceAPictureWasGiven)
{
	Thumbnails thumbnails;

	thumbnails.SendAndWait(Join({kH264OpenGopParameterSetsThreeReferences, kH264OpenGopKeyframeWithoutSei}), 0);
	thumbnails.SendAndWait(kH264OpenGopKeyframeWithoutSei, 1000);
	thumbnails.SendAndWait(kH264OpenGopKeyframe, 2000);
	thumbnails.SendAndWait(kH264OpenGopKeyframeWithoutSei, 3000);
	thumbnails.SendAndWait(kH264OpenGopKeyframeWithoutSei, 4000);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{2000, {100}}, {3000, {100}}, {4000, {100}}}));
}

// A publisher can flag any packet as a keyframe. The decoder of the pipeline decodes each of them and so gives
// their pictures; this one would build them on references it never decoded.
TEST(TranscodeThumbnailer, KeyPacketThatIsNotIntraCodedHandsTheTrackBack)
{
	const std::vector<uint8_t> parameter_sets(kH264Idr.begin(), kH264Idr.begin() + 34);

	// A P slice, the same marked as a recovery point, and behind or in front of an I slice
	for (auto &not_intra : {kH264PSlice, Join({kH264RecoveryPointSei, kH264PSlice}), Join({kH264NonIdr, kH264PSlice}), Join({kH264PSlice, kH264NonIdr})})
	{
		SCOPED_TRACE(ov::String::FormatString("%zu bytes", not_intra.size()).CStr());

		Thumbnails thumbnails;

		thumbnails.SendAndWait(kH264Idr, 0);
		thumbnails.SendAndHold(kH264NonIdr, 1000);

		// In a slot that has its image: the packet is refused although its picture would not be decoded
		thumbnails.Send(not_intra, 1200);
		thumbnails.Send(kH264Idr, 3000);
		EXPECT_FALSE(thumbnails.Refused());

		thumbnails.Hold(false);
		ASSERT_TRUE(ome_test::WaitUntil([&]() { return thumbnails.Refused(); }, 10000));
		EXPECT_EQ(thumbnails.Send(kH264Idr, 4000).use_count(), 1);
		thumbnails.Stop();

		// The keyframe that waited behind it is not decoded
		EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1000, {100}}}));
	}

	Thumbnails thumbnails;
	thumbnails.Send(Join({parameter_sets, kH264PSlice}), 0);
	ASSERT_TRUE(ome_test::WaitUntil([&]() { return thumbnails.Refused(); }, 10000));
	EXPECT_TRUE(thumbnails.Images().empty());
}

TEST(TranscodeThumbnailer, PipelineStartsAfterAHandBackAtAnIdrPictureOrARecoveryPoint)
{
	const std::vector<uint8_t> parameter_sets(kH264Idr.begin(), kH264Idr.begin() + 34);
	auto input_track = MakeInputTrack();
	input_track->SetDecoderConfigurationRecord(MakeAvcRecord(parameter_sets));

	auto start = [&](const std::vector<uint8_t> &data, MediaPacketFlag flag = MediaPacketFlag::Key) {
		return TranscodeThumbnailer::GetPipelineStart(Thumbnails::MakePacket(data, 0, flag), input_track);
	};

	auto idr = Thumbnails::MakePacket(kH264Idr, 0, MediaPacketFlag::Key);
	EXPECT_EQ(TranscodeThumbnailer::GetPipelineStart(idr, input_track), idr);
	auto slices = Thumbnails::MakePacket(Slices(kH264Idr), 0, MediaPacketFlag::Key);
	EXPECT_EQ(TranscodeThumbnailer::GetPipelineStart(slices, input_track), slices);

	// As x264 with intra-refresh=1 sends its keyframes, and an open GOP its I pictures. The parameter sets of the
	// avcC go in front: the media router puts them in front of IDR pictures only.
	for (auto &recovery_point : {Join({kH264RecoveryPointSei, kH264PSlice}), kH264OpenGopKeyframe})
	{
		auto packet = start(recovery_point);
		ASSERT_NE(packet, nullptr);
		auto expected = Join({parameter_sets, recovery_point});
		EXPECT_TRUE(packet->GetData()->IsEqual(expected.data(), expected.size()));
		EXPECT_EQ(packet->GetFlag(), MediaPacketFlag::Key);
	}

	EXPECT_EQ(start(kH264PSlice), nullptr);
	EXPECT_EQ(start(kH264NonIdr), nullptr);
	EXPECT_EQ(start(kH264OpenGopKeyframeWithoutSei), nullptr);
	EXPECT_EQ(start(kH264OpenGopParameterSets), nullptr);
	EXPECT_EQ(start({}), nullptr);
	// The decoder of a keyframe-only track drops the other packets
	EXPECT_EQ(start(kH264Idr, MediaPacketFlag::NoFlag), nullptr);
	// Without an avcC there are no parameter sets to start with
	EXPECT_EQ(TranscodeThumbnailer::GetPipelineStart(Thumbnails::MakePacket(kH264OpenGopKeyframe, 0, MediaPacketFlag::Key), MakeInputTrack()), nullptr);
}

TEST(TranscodeThumbnailer, KeyframesThatAreIntraCodedDoNotHandTheTrackBack)
{
	// Rejected, concealed or held back keyframes, and slice units whose type cannot be read
	auto damaged = kH264Idr;
	damaged[damaged.size() - 6] ^= 0xff;

	Thumbnails thumbnails;

	thumbnails.SendAndWait(kH264Truncated, 1000);
	thumbnails.SendAndWait(kH264Idr, 2000);
	thumbnails.SendAndWait(damaged, 3000);
	thumbnails.SendAndWait(kH264NonIdr, 4000);
	thumbnails.SendAndWait(Join({kH264Dark, kH264Bright}), 5000);
	thumbnails.SendAndWait(Join({kH264OpenGopParameterSetsThreeReferences, kH264OpenGopKeyframe}), 6000);
	thumbnails.SendAndWait(kH264OpenGopKeyframeWithoutSei, 7000);
	thumbnails.SendAndWait({0x00, 0x00, 0x00, 0x01, 0x41}, 8000);
	thumbnails.SendAndWait({0x00, 0x00, 0x00, 0x01, 0x41, 0x8b}, 9000);
	EXPECT_FALSE(thumbnails.Refused());
	thumbnails.Stop();

	auto images = thumbnails.Images();
	ASSERT_GE(images.size(), 5u);
	EXPECT_EQ(images.front().first, 2000);
	EXPECT_EQ(images.back().first, 7000);

	// VP8 has no slices; bytes that read as a P slice of H.264 mean nothing there
	Thumbnails vp8({MakeOutputTrack(100, 1.0)}, MakeInputTrack(cmn::MediaCodecId::Vp8));
	vp8.SendAndWait(Join({kVp8, kH264PSlice}), 0);
	EXPECT_FALSE(vp8.Refused());
}

TEST(TranscodeThumbnailer, DecodeErrorIsReportedAndKeepsTheSlotOpen)
{
	Thumbnails thumbnails({MakeOutputTrack(100, 1.0)}, MakeInputTrack(cmn::MediaCodecId::Vp8));

	// FFmpeg refuses a key frame of width 0 with an error that is not "invalid data"
	auto no_width = kVp8;
	no_width[6] = 0;

	thumbnails.SendAndWait(kVp8, 0);
	thumbnails.SendAndWait(no_width, 1000);
	thumbnails.SendAndWait(kVp8, 1200);
	thumbnails.Stop();

	EXPECT_EQ(thumbnails.Images(), (std::vector<Image>{{0, {100}}, {1200, {100}}}));

	auto results = thumbnails.Results();
	ASSERT_EQ(results.size(), 3u);
	EXPECT_EQ(results[1].result, TranscodeResult::DataError);
}

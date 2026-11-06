//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <base/info/application.h>
#include <base/info/stream.h>
#include <modules/bitstream/h264/h264_decoder_configuration_record.h>
#include <modules/bitstream/h264/h264_parser.h>
#include <modules/ffmpeg/compat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

#include "../../filter/filter_fps.h"
#include "../../transcoder_filter.h"
#include "decoder_avcodec_video.h"

// Keyframe-only decoding for image outputs, decoder and FPS filters together. Holding keyframes
// must give every output slot the keyframe that decoding all of them gives, emit each slot when
// the first keyframe of a later slot arrives, and decode only the keyframes an output can use.

namespace
{
	constexpr int kFps			 = 30;
	constexpr int64_t kTimescale = 90000;
	const cmn::Timebase kTimebase(1, kTimescale);

	struct Size
	{
		int width;
		int height;
	};
	constexpr Size kSizes[] = {{160, 96}, {128, 80}};

	struct Keyframe
	{
		int64_t pts;
		int64_t dts;
		std::vector<uint8_t> data;
	};

	// Each keyframe picture carries its index in the list it was made from (up to 255): the U plane the
	// sixteens, the V plane the rest, in steps the codec keeps apart
	constexpr int kMarkBase = 10;
	constexpr int kMarkStep = 12;
	constexpr int kNoMark	= -1;

	// The mark of a picture, kNoMark if it has none
	int ReadMark(const std::shared_ptr<MediaFrame> &frame)
	{
		auto av_frame = static_cast<AVFrame *>(frame->GetPrivData());
		auto digit	  = [&](int plane) {
			 int value = av_frame->data[plane][(av_frame->height / 4) * av_frame->linesize[plane] + av_frame->width / 4];
			 int index = static_cast<int>(std::lround(static_cast<double>(value - kMarkBase) / kMarkStep));
			 return (index < 0 || index > 15 || std::abs(value - (kMarkBase + kMarkStep * index)) > 3) ? kNoMark : index;
		};

		int high = digit(1);
		int low	 = digit(2);
		return (high == kNoMark || low == kNoMark) ? kNoMark : high * 16 + low;
	}

	// mark: the index to write, kNoMark for a flat picture
	void FillPicture(AVFrame *frame, int64_t index, int mark)
	{
		::av_frame_make_writable(frame);
		for (int y = 0; y < frame->height; y++)
		{
			::memset(frame->data[0] + y * frame->linesize[0], static_cast<int>((index * 3 + y) & 0xff), frame->width);
		}
		int u = (mark == kNoMark) ? 128 : kMarkBase + kMarkStep * (mark / 16);
		int v = (mark == kNoMark) ? 128 : kMarkBase + kMarkStep * (mark % 16);
		for (int y = 0; y < frame->height / 2; y++)
		{
			::memset(frame->data[1] + y * frame->linesize[1], u, frame->width / 2);
			::memset(frame->data[2] + y * frame->linesize[2], v, frame->width / 2);
		}
	}

	AVFrame *AllocPicture(const Size &size)
	{
		AVFrame *frame = ::av_frame_alloc();
		frame->width   = size.width;
		frame->height  = size.height;
		frame->format  = AV_PIX_FMT_YUV420P;
		::av_frame_get_buffer(frame, 0);
		return frame;
	}

	// One keyframe per time, in a stream of 30 fps pictures. H.264 has B-frames, so the codec holds back its output
	// when it is fed keyframes only. The picture size changes at each of size_change_seconds (a new encoder starts).
	std::vector<Keyframe> EncodeKeyframes(const std::vector<double> &key_seconds, const std::vector<double> &size_change_seconds = {},
										  cmn::MediaCodecId codec_id = cmn::MediaCodecId::H264, bool interlaced = false)
	{
		std::vector<Keyframe> keyframes;

		const AVCodec *codec = (codec_id == cmn::MediaCodecId::Vp8) ? ::avcodec_find_encoder(AV_CODEC_ID_VP8) : ::avcodec_find_encoder_by_name("libx264");
		if (codec == nullptr)
		{
			return keyframes;
		}

		AVCodecContext *context = nullptr;
		AVFrame *frame			= nullptr;
		auto open				= [&](int size_index) {
			   ::avcodec_free_context(&context);
			   ::av_frame_free(&frame);

			   context				 = ::avcodec_alloc_context3(codec);
			   context->width		 = kSizes[size_index].width;
			   context->height		 = kSizes[size_index].height;
			   context->pix_fmt		 = AV_PIX_FMT_YUV420P;
			   context->time_base	 = {1, static_cast<int>(kTimescale)};
			   context->framerate	 = {kFps, 1};
			   context->gop_size	 = 10000;
			   context->max_b_frames = 2;
			   ::av_opt_set(context->priv_data, "preset", "veryfast", 0);
			   ::av_opt_set_int(context->priv_data, "forced-idr", 1, 0);
			   ::av_opt_set(context->priv_data, "x264-params", interlaced ? "scenecut=0:tff=1" : "scenecut=0", 0);
			   ::av_opt_set_int(context->priv_data, "lag-in-frames", 0, 0);

			   if (::avcodec_open2(context, codec, nullptr) < 0)
			   {
				   ::avcodec_free_context(&context);
				   return false;
			   }

			   frame = AllocPicture(kSizes[size_index]);
			   return true;
		};

		if (open(0) == false)
		{
			return keyframes;
		}

		std::map<int64_t, int> key_indexes;
		for (size_t i = 0; i < key_seconds.size(); i++)
		{
			key_indexes[std::llround(key_seconds[i] * kFps)] = static_cast<int>(i);
		}
		std::set<int64_t> change_indexes;
		for (auto seconds : size_change_seconds)
		{
			change_indexes.insert(std::llround(seconds * kFps));
		}
		int size_index = 0;

		AVPacket *packet = ::av_packet_alloc();
		auto collect	 = [&]() {
			while (::avcodec_receive_packet(context, packet) == 0)
			{
				if (packet->flags & AV_PKT_FLAG_KEY)
				{
					keyframes.push_back({packet->pts, packet->dts, std::vector<uint8_t>(packet->data, packet->data + packet->size)});
				}
				::av_packet_unref(packet);
			}
		};

		const int64_t last_index = key_indexes.rbegin()->first + kFps;
		for (int64_t index = 0; index <= last_index; index++)
		{
			if (change_indexes.count(index) > 0)
			{
				::avcodec_send_frame(context, nullptr);
				collect();

				size_index = 1 - size_index;
				if (open(size_index) == false)
				{
					break;
				}
			}

			auto key = key_indexes.find(index);
			FillPicture(frame, index, (key != key_indexes.end()) ? key->second : kNoMark);
			frame->pts		 = index * (kTimescale / kFps);
			frame->pict_type = (key != key_indexes.end()) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

			::avcodec_send_frame(context, frame);
			collect();
		}

		if (context != nullptr)
		{
			::avcodec_send_frame(context, nullptr);
			collect();
		}

		::av_packet_free(&packet);
		::av_frame_free(&frame);
		::avcodec_free_context(&context);

		return keyframes;
	}

	std::vector<double> Periodic(double interval, double duration)
	{
		std::vector<double> seconds;
		for (int i = 0; i * interval <= duration + 1e-9; i++)
		{
			seconds.push_back(i * interval);
		}
		return seconds;
	}

	// The avcC a stream's setup would carry: the SPS and PPS of its first keyframe
	std::shared_ptr<DecoderConfigurationRecord> MakeAvcRecord(const Keyframe &keyframe)
	{
		auto record = std::make_shared<AVCDecoderConfigurationRecord>();

		const uint8_t *data = keyframe.data.data();
		for (const auto &index : H264Parser::FindNaluIndexes(data, keyframe.data.size()))
		{
			auto nalu = std::make_shared<ov::Data>(data + index._payload_offset, index._payload_size);
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

	// The decoder only names its queue after the application
	struct TestApplication : public info::Application
	{
		static const info::Application &Get()
		{
			return GetInvalidApplication();
		}
	};

	std::shared_ptr<MediaPacket> ToPacket(const Keyframe &keyframe, cmn::MediaCodecId codec_id)
	{
		bool vp8 = (codec_id == cmn::MediaCodecId::Vp8);
		return std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, keyframe.data.data(), static_cast<int32_t>(keyframe.data.size()),
											 keyframe.pts, keyframe.dts, 0, MediaPacketFlag::Key,
											 vp8 ? cmn::BitstreamFormat::VP8 : cmn::BitstreamFormat::H264_ANNEXB,
											 vp8 ? cmn::PacketType::RAW : cmn::PacketType::NALU);
	}

	struct Emitted
	{
		int64_t slot;
		int source;				// index of the keyframe the image shows
		size_t keyframe_index;	// the keyframe being fed when the slot came out
	};

	using ImageCount = std::function<uint64_t(size_t output, uint64_t own)>;

	int64_t SlotOf(int64_t pts, double framerate)
	{
		return cmn::Rational::Rescale(pts, cmn::Rational(kTimebase.GetNum(), kTimebase.GetDen()), cmn::Rational::FromDouble(framerate, std::numeric_limits<int32_t>::max()).Invert());
	}

	std::shared_ptr<MediaTrack> InputTrack(std::shared_ptr<DecoderConfigurationRecord> avcc, cmn::MediaCodecId codec_id)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(0);
		track->SetMediaType(cmn::MediaType::Video);
		track->SetCodecId(codec_id);
		track->SetCodecModuleId(cmn::MediaCodecModuleId::DEFAULT);
		track->SetTimeBase(1, kTimescale);
		track->SetKeyframeDecodeOnly(true);
		track->SetThreadCount(2);
		track->SetDecoderConfigurationRecord(avcc);
		return track;
	}

	// The decoder followed by one FPS filter per image output, as OnDecodedFrame feeds them
	class Pipeline
	{
	public:
		Pipeline(std::vector<double> framerates, std::shared_ptr<DecoderConfigurationRecord> avcc, cmn::MediaCodecId codec_id = cmn::MediaCodecId::H264)
			: _framerates(framerates),
			  _codec_id(codec_id)
		{
			_track = InputTrack(avcc, codec_id);

			for (auto framerate : framerates)
			{
				auto filter = std::make_unique<FilterFps>();
				filter->SetInputTimebase(_track->GetTimeBase());
				filter->SetInputFrameRate(kFps);
				filter->SetOutputFrameRate(framerate);
				filter->SetSkipFrames(FilterFps::SkipFramesDisabled);
				filter->SetOutputFrameCopyMode(FilterFps::OutputFrameCopyMode::ShallowCopy);
				_filters.push_back(std::move(filter));
				emitted.emplace_back();
			}

			_decoder = std::make_shared<AVCodecVideoDecoder>(info::Stream(TestApplication::Get(), StreamSourceType::Transcoder), codec_id);
		}

		~Pipeline()
		{
			_decoder->Stop();
		}

		// select: the decoder may skip keyframes. images: what to report as the number of images output i has
		// produced, given the filter's own count (which is reported by default)
		bool Start(bool select, ImageCount images = nullptr)
		{
			std::vector<KeyframeSelector::ImageOutput> outputs;
			for (size_t i = 0; select && i < _framerates.size(); i++)
			{
				outputs.push_back({_framerates[i], [this, i, images]() -> uint64_t {
									   std::lock_guard<std::mutex> lock(_mutex);
									   uint64_t own = emitted[i].size();
									   return (images != nullptr) ? images(i, own) : own;
								   }});
			}

			_decoder->SetImageOutputs(std::move(outputs));
			_decoder->SetCompleteHandler([this](TranscodeResult result, int32_t, std::shared_ptr<MediaFrame> frame) {
				OnFrame(result, std::move(frame));
			});

			return _decoder->Configure(_track);
		}

		// With wait, returns once the decoder handed out a frame (decoded or stand-in) for it
		bool Feed(size_t index, const Keyframe &keyframe, bool wait, int timeout_ms = 5000)
		{
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_feeding = index;
			}

			_decoder->SendBuffer(ToPacket(keyframe, _codec_id));
			if (wait == false)
			{
				return true;
			}

			std::unique_lock<std::mutex> lock(_mutex);
			return _cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() { return _handed_out.count(keyframe.pts) > 0; });
		}

		// The decoder drops what is still queued when it is drained, so the queue is let run dry first:
		// outputs: how many frames the codec has handed out by then (all but its delay); settle: until it
		// has handed out nothing new for that long
		void Finish(size_t index, size_t outputs = 0, bool settle = false)
		{
			{
				std::unique_lock<std::mutex> lock(_mutex);
				_feeding = index;
				_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return _handed_out_count >= outputs; });

				size_t seen;
				while (settle && (seen = _handed_out_count, _cv.wait_for(lock, std::chrono::milliseconds(300), [&]() { return _handed_out_count != seen; })))
				{
				}
			}

			_decoder->DrainAndStop();
		}

		std::vector<std::vector<Emitted>> emitted;
		// The indexes of the keyframes whose pictures were decoded (a stand-in shows an earlier one)
		std::set<int> decoded;

	private:
		void OnFrame(TranscodeResult result, std::shared_ptr<MediaFrame> frame)
		{
			if (frame == nullptr || (result != TranscodeResult::DataReady && result != TranscodeResult::FormatChanged))
			{
				return;
			}

			std::lock_guard<std::mutex> lock(_mutex);

			_handed_out.insert(frame->GetPts());
			_handed_out_count++;
			if (auto mark = ReadMark(frame); mark != kNoMark)
			{
				decoded.insert(mark);
			}

			for (size_t i = 0; i < _filters.size(); i++)
			{
				_filters[i]->Push(frame->CloneFrame());
				while (auto output = _filters[i]->Pop())
				{
					emitted[i].push_back({SlotOf(output->GetPts(), _framerates[i]), ReadMark(output), _feeding});
				}
			}

			_cv.notify_all();
		}

		std::vector<double> _framerates;
		cmn::MediaCodecId _codec_id;
		std::shared_ptr<MediaTrack> _track;
		std::shared_ptr<AVCodecVideoDecoder> _decoder;
		std::vector<std::unique_ptr<FilterFps>> _filters;

		std::mutex _mutex;
		std::condition_variable _cv;
		std::set<int64_t> _handed_out;
		size_t _handed_out_count = 0;
		size_t _feeding			 = 0;
	};

	// Slot -> index of the keyframe shown
	std::map<int64_t, int> Sources(const std::vector<Emitted> &emitted)
	{
		std::map<int64_t, int> sources;
		for (auto &e : emitted)
		{
			sources[e.slot] = e.source;
		}
		return sources;
	}

	// Keyframes every decoded keyframe run decodes: the first, one that skips a slot of some output,
	// and the last of its slot for some output
	std::set<int> ExpectedDecoded(const std::vector<Keyframe> &keyframes, const std::vector<double> &framerates)
	{
		std::set<int> decoded;
		for (size_t i = 0; i < keyframes.size(); i++)
		{
			bool needed = (i == 0);
			for (auto framerate : framerates)
			{
				needed = needed || ((i > 0) && (SlotOf(keyframes[i].pts, framerate) - SlotOf(keyframes[i - 1].pts, framerate) > 1));
				needed = needed || ((i + 1 < keyframes.size()) && (SlotOf(keyframes[i + 1].pts, framerate) != SlotOf(keyframes[i].pts, framerate)));
			}
			if (needed)
			{
				decoded.insert(static_cast<int>(i));
			}
		}
		return decoded;
	}

	// Frames a codec fed keyframes only may still hold back (B-frames, frame threads)
	constexpr size_t kCodecDelay = 4;

	// Feeds all keyframes to a decoder that decodes every one of them. pictures: how many of them give one
	std::unique_ptr<Pipeline> DecodeAll(const std::vector<double> &framerates, const std::vector<Keyframe> &keyframes, size_t pictures = SIZE_MAX)
	{
		pictures = std::min(pictures, keyframes.size());

		auto all = std::make_unique<Pipeline>(framerates, MakeAvcRecord(keyframes[0]));
		EXPECT_TRUE(all->Start(false));
		for (size_t i = 0; i < keyframes.size(); i++)
		{
			all->Feed(i, keyframes[i], false);
		}
		all->Finish(keyframes.size(), pictures - std::min(pictures, kCodecDelay));

		return all;
	}

	// While keyframes are selected the codec hands a frame out for each one at once, so the next is fed after it.
	// A decoder that falls back to the codec's own pace is fed without waiting: from the first frame that
	// does not come (UntilMiss), or from the start (Never).
	enum class Wait
	{
		Each,
		UntilMiss,
		Never
	};

	// Feeds all keyframes to a decoder that selects them
	std::unique_ptr<Pipeline> DecodeSelected(const std::vector<double> &framerates, const std::vector<Keyframe> &keyframes, std::shared_ptr<DecoderConfigurationRecord> avcc,
											 ImageCount images = nullptr, Wait wait = Wait::Each, size_t pictures = SIZE_MAX)
	{
		pictures = std::min(pictures, keyframes.size());

		auto held = std::make_unique<Pipeline>(framerates, avcc);
		EXPECT_TRUE(held->Start(true, images));
		bool waiting = (wait != Wait::Never);
		for (size_t i = 0; i < keyframes.size(); i++)
		{
			bool fed = held->Feed(i, keyframes[i], waiting, (wait == Wait::UntilMiss) ? 2000 : 5000);
			if (wait == Wait::Each)
			{
				EXPECT_TRUE(fed) << "no frame for keyframe " << i;
			}
			waiting = waiting && fed;
		}
		held->Finish(keyframes.size(), (wait == Wait::Each) ? 0 : pictures - std::min(pictures, kCodecDelay), wait != Wait::Each);

		return held;
	}

	// The selecting run gives every output slot the keyframe the run decoding all keyframes gives, emits it
	// when the first keyframe of a later slot arrives, and decodes only what an output can use
	void RunAndCompare(const std::vector<double> &framerates, const std::vector<double> &key_seconds, bool expect_savings)
	{
		auto keyframes = EncodeKeyframes(key_seconds);
		ASSERT_EQ(keyframes.size(), key_seconds.size());

		auto all = DecodeAll(framerates, keyframes);
		EXPECT_EQ(all->decoded.size(), keyframes.size());

		auto held = DecodeSelected(framerates, keyframes, MakeAvcRecord(keyframes[0]));

		EXPECT_EQ(held->decoded, ExpectedDecoded(keyframes, framerates));
		if (expect_savings == true)
		{
			EXPECT_LT(held->decoded.size(), all->decoded.size());
		}
		else
		{
			// Every keyframe skips a slot; only the last, which an output has no use for, is left out
			EXPECT_GE(held->decoded.size() + 1, all->decoded.size());
		}

		for (size_t f = 0; f < framerates.size(); f++)
		{
			ASSERT_FALSE(held->emitted[f].empty());
			EXPECT_EQ(Sources(held->emitted[f]), Sources(all->emitted[f])) << "rate " << framerates[f];

			for (auto &e : held->emitted[f])
			{
				// The last keyframe at or before the slot
				int expected_source = -1;
				for (size_t i = 0; i < keyframes.size(); i++)
				{
					if (SlotOf(keyframes[i].pts, framerates[f]) <= e.slot)
					{
						expected_source = static_cast<int>(i);
					}
				}
				EXPECT_EQ(e.source, expected_source) << "rate " << framerates[f] << " slot " << e.slot;

				// Out with the first keyframe of a later slot, not later as the codec's delay would have it
				size_t first_later = keyframes.size();
				for (size_t i = 0; i < keyframes.size(); i++)
				{
					if (SlotOf(keyframes[i].pts, framerates[f]) > e.slot)
					{
						first_later = i;
						break;
					}
				}
				EXPECT_EQ(e.keyframe_index, first_later) << "rate " << framerates[f] << " slot " << e.slot;
			}
		}
	}
}  // namespace

TEST(AVCodecVideoDecoderKeyframeSelection, SlotPicturesMatchDecodingEveryKeyframe)
{
	struct Case
	{
		std::vector<double> framerates;
		double interval;
		double duration;
		bool savings;
	};

	// Image rates of 1, 0.5 and 0.1 fps against keyframe intervals shorter and longer than a slot
	const std::vector<Case> cases = {
		{{1.0}, 0.4, 12, true},
		{{1.0}, 1.5, 12, false},
		{{0.5}, 0.7, 24, true},
		{{0.5}, 2.5, 24, false},
		{{static_cast<float>(0.1)}, 2.0, 80, true},
		{{static_cast<float>(0.1)}, 4.5, 80, true},
		{{static_cast<float>(0.1)}, 12.0, 120, false},
		{{1.0, 0.5, static_cast<float>(0.1)}, 0.7, 40, true},
		{{static_cast<float>(0.1), static_cast<float>(0.25)}, 2.0, 80, true},
	};

	for (const auto &c : cases)
	{
		SCOPED_TRACE("rates " + std::to_string(c.framerates.size()) + " first " + std::to_string(c.framerates[0]) + " interval " + std::to_string(c.interval));
		RunAndCompare(c.framerates, Periodic(c.interval, c.duration), c.savings);
	}
}

TEST(AVCodecVideoDecoderKeyframeSelection, EmptySlotsAreFilledAsWithoutSelection)
{
	// An empty slot (25-35 s) and a 10 s gap between keyframes inside slots
	RunAndCompare({static_cast<float>(0.1)}, {0, 2, 4.5, 9, 12, 14.9, 15.1, 18, 24, 36, 38, 44.9, 47, 49.9, 52, 55, 58}, true);
}

namespace
{
	const std::vector<double> kOneRate = {static_cast<float>(0.1)};

	struct Runs
	{
		std::unique_ptr<Pipeline> all;
		std::unique_ptr<Pipeline> held;
	};

	// A stream whose timeline breaks at keyframe run_start: its pts are moved back by restamp_seconds from there on.
	// The held keyframe before the break is dropped, the one after it is decoded at once as the first of a new run,
	// and holding resumes after it.
	Runs RunWithBreak(const std::vector<double> &key_seconds, size_t run_start, double restamp_seconds)
	{
		auto keyframes = EncodeKeyframes(key_seconds);
		EXPECT_EQ(keyframes.size(), key_seconds.size());
		for (size_t i = run_start; i < keyframes.size(); i++)
		{
			keyframes[i].pts -= std::llround(restamp_seconds * kTimescale);
			keyframes[i].dts -= std::llround(restamp_seconds * kTimescale);
		}

		Runs runs{DecodeAll(kOneRate, keyframes), DecodeSelected(kOneRate, keyframes, MakeAvcRecord(keyframes[0]))};

		std::set<int> expected;
		for (auto [first, end] : {std::pair<size_t, size_t>{0, run_start}, {run_start, keyframes.size()}})
		{
			for (auto index : ExpectedDecoded({keyframes.begin() + first, keyframes.begin() + end}, kOneRate))
			{
				expected.insert(index + static_cast<int>(first));
			}
		}
		EXPECT_EQ(runs.held->decoded, expected);
		EXPECT_EQ(runs.held->decoded.count(run_start - 1), 0u);
		EXPECT_EQ(runs.held->decoded.count(run_start), 1u);

		return runs;
	}
}  // namespace

TEST(AVCodecVideoDecoderKeyframeSelection, ATimestampJumpRestartsSelectionAndHoldingResumes)
{
	// Every 2 s up to 40 s, then a 30 s jump forward
	std::vector<double> key_seconds = Periodic(2, 40);
	for (double seconds = 70; seconds <= 110; seconds += 2)
	{
		key_seconds.push_back(seconds);
	}

	auto runs = RunWithBreak(key_seconds, 21, 0);
	EXPECT_LT(runs.held->decoded.size(), runs.all->decoded.size() - 10);

	// Slots 4-6 (the dropped keyframe and the empty slots up to the jump) show an earlier picture, the rest the same one
	auto held_sources = Sources(runs.held->emitted[0]);
	auto all_sources  = Sources(runs.all->emitted[0]);
	ASSERT_EQ(held_sources.size(), all_sources.size());
	for (auto &[slot, source] : all_sources)
	{
		if (slot >= 4 && slot <= 6)
		{
			EXPECT_LT(held_sources[slot], source) << "slot " << slot;
			EXPECT_NE(held_sources[slot], kNoMark) << "slot " << slot;
		}
		else
		{
			EXPECT_EQ(held_sources[slot], source) << "slot " << slot;
		}
	}
}

TEST(AVCodecVideoDecoderKeyframeSelection, AStepBackRestartsSelectionAndHoldingResumes)
{
	// Every 2 s up to 40 s, then the stream goes back to 34 s
	auto runs = RunWithBreak(Periodic(2, 80), 21, 8);

	// The FPS filter drops the frames behind its output position, with or without selection
	EXPECT_EQ(Sources(runs.held->emitted[0]), Sources(runs.all->emitted[0]));
}

TEST(AVCodecVideoDecoderKeyframeSelection, AnOutputThatStopsProducingRestartsSelectionThenGivesUp)
{
	// Slots of 10 s, a keyframe every 2 s for 140 s: a slot boundary every 5 keyframes
	const std::vector<double> framerates = {static_cast<float>(0.1)};
	auto keyframes						 = EncodeKeyframes(Periodic(2, 140));
	const auto planned					 = ExpectedDecoded(keyframes, framerates);

	// Control: the output produces its images, so only the keyframes the plan names are decoded
	auto working = DecodeSelected(framerates, keyframes, MakeAvcRecord(keyframes[0]));
	EXPECT_EQ(working->decoded, planned);

	// No image is ever counted: after two boundaries without one the held keyframe is dropped and the next is
	// decoded at once, three times; the fourth time the stream goes back to decoding every keyframe
	auto stalled = DecodeSelected(framerates, keyframes, MakeAvcRecord(keyframes[0]), [](size_t, uint64_t) -> uint64_t { return 0; }, Wait::UntilMiss);
	std::set<int> extra;
	for (auto i : stalled->decoded)
	{
		if (planned.count(i) == 0)
		{
			extra.insert(i);
		}
	}
	EXPECT_GE(extra.size(), 3u);
	for (size_t i = keyframes.size() - 10; i < keyframes.size() - 1; i++)
	{
		EXPECT_EQ(stalled->decoded.count(static_cast<int>(i)), 1u) << i;
		EXPECT_EQ(working->decoded.count(static_cast<int>(i)), planned.count(static_cast<int>(i))) << i;
	}

	// The images keep coming
	EXPECT_EQ(Sources(stalled->emitted[0]).size(), Sources(working->emitted[0]).size());
}

TEST(AVCodecVideoDecoderKeyframeSelection, OneOutputThatStopsProducingAmongSeveralGivesUpToo)
{
	// 0.1 and 0.5 fps, a keyframe every 2 s. The 0.5 fps output never reports an image; the other one does all the time
	const std::vector<double> framerates = {static_cast<float>(0.1), static_cast<float>(0.5)};
	auto keyframes						 = EncodeKeyframes(Periodic(2, 200));

	auto all	 = DecodeAll(framerates, keyframes);
	auto stalled = DecodeSelected(framerates, keyframes, MakeAvcRecord(keyframes[0]), [](size_t output, uint64_t own) { return (output == 1) ? 0 : own; }, Wait::UntilMiss);

	// The restarts it causes are counted on its own, so the other output's images do not keep them from ending
	for (size_t i = 30; i < keyframes.size() - 1; i++)
	{
		EXPECT_EQ(stalled->decoded.count(static_cast<int>(i)), 1u) << i;
	}
	for (size_t f = 0; f < framerates.size(); f++)
	{
		auto stalled_sources = Sources(stalled->emitted[f]);
		auto all_sources	 = Sources(all->emitted[f]);
		ASSERT_EQ(stalled_sources.size(), all_sources.size());
		for (auto &[slot, source] : all_sources)
		{
			if (slot / framerates[f] >= 60)
			{
				EXPECT_EQ(stalled_sources[slot], source) << "rate " << framerates[f] << " slot " << slot;
			}
		}
	}
}

namespace
{
	// A keyframe the decoder makes no picture of
	void Corrupt(Keyframe &keyframe)
	{
		keyframe.data.assign(64, 0x55);
	}

	// The keyframes that are the last of their slot, the ones a selecting decoder decodes
	std::vector<size_t> SlotFinals(const std::vector<Keyframe> &keyframes, double framerate)
	{
		std::vector<size_t> finals;
		for (size_t i = 0; i + 1 < keyframes.size(); i++)
		{
			if (SlotOf(keyframes[i + 1].pts, framerate) != SlotOf(keyframes[i].pts, framerate))
			{
				finals.push_back(i);
			}
		}
		return finals;
	}
}  // namespace

TEST(AVCodecVideoDecoderKeyframeSelection, SlotFinalKeyframesThatCannotBeDecodedEndSelection)
{
	// 0.1 fps, a keyframe every 2 s: the last keyframe of every slot is corrupt, the others are fine
	auto keyframes = EncodeKeyframes(Periodic(2, 200));
	auto finals	   = SlotFinals(keyframes, kOneRate[0]);
	for (auto i : finals)
	{
		Corrupt(keyframes[i]);
	}
	const size_t healthy = keyframes.size() - finals.size();

	auto all = DecodeAll(kOneRate, keyframes, healthy);
	ASSERT_EQ(all->decoded.size(), healthy);

	auto held = DecodeSelected(kOneRate, keyframes, MakeAvcRecord(keyframes[0]), nullptr, Wait::UntilMiss, 0);

	// Each failed decode restarts selection; after the fourth the stream is decoded as before, so from there on the
	// slots show what decoding every keyframe shows, instead of the first picture for the rest of the stream
	auto held_sources = Sources(held->emitted[0]);
	auto all_sources  = Sources(all->emitted[0]);
	ASSERT_EQ(held_sources.size(), all_sources.size());
	for (auto &[slot, source] : all_sources)
	{
		if (slot >= 10)
		{
			EXPECT_EQ(held_sources[slot], source) << "slot " << slot;
		}
	}
	for (size_t i = keyframes.size() - 10; i < keyframes.size() - 1; i++)
	{
		EXPECT_EQ(held->decoded.count(static_cast<int>(i)), all->decoded.count(static_cast<int>(i))) << i;
	}
}

TEST(AVCodecVideoDecoderKeyframeSelection, OneSlotFinalKeyframeThatCannotBeDecodedCostsOneSlot)
{
	// The last keyframe of slot 3 (34 s) is corrupt
	auto keyframes = EncodeKeyframes(Periodic(2, 120));
	auto finals	   = SlotFinals(keyframes, kOneRate[0]);
	const size_t corrupt = finals[3];
	ASSERT_EQ(keyframes[corrupt].pts, 34 * kTimescale);
	Corrupt(keyframes[corrupt]);

	auto all  = DecodeAll(kOneRate, keyframes, keyframes.size() - 1);
	auto held = DecodeSelected(kOneRate, keyframes, MakeAvcRecord(keyframes[0]), nullptr, Wait::UntilMiss, keyframes.size() - 1);

	// Slot 3 shows an earlier picture. Selection went on: every other slot is right, and the keyframes
	// in the last slots are still held, not all decoded
	auto held_sources = Sources(held->emitted[0]);
	auto all_sources  = Sources(all->emitted[0]);
	ASSERT_EQ(held_sources.size(), all_sources.size());
	for (auto &[slot, source] : all_sources)
	{
		if (slot == 3)
		{
			EXPECT_LT(held_sources[slot], source);
		}
		else
		{
			EXPECT_EQ(held_sources[slot], source) << "slot " << slot;
		}
	}
	EXPECT_EQ(held->decoded.count(static_cast<int>(keyframes.size() - 3)), 0u);
	EXPECT_LT(held->decoded.size(), all->decoded.size() - 20);
}

TEST(AVCodecVideoDecoderKeyframeSelection, AStreamThatMayBeInterlacedIsDecodedKeyframeByKeyframe)
{
	const std::vector<double> framerates = {static_cast<float>(0.1)};
	auto keyframes						 = EncodeKeyframes(Periodic(2, 60));
	auto expected						 = ExpectedDecoded(keyframes, framerates);
	ASSERT_LT(expected.size(), keyframes.size());

	// An avcC with a progressive SPS: selection is on
	EXPECT_EQ(DecodeSelected(framerates, keyframes, MakeAvcRecord(keyframes[0]))->decoded, expected);

	// No avcC at all: every keyframe is decoded
	auto unknown = DecodeSelected(framerates, keyframes, nullptr, nullptr, Wait::Never);
	EXPECT_EQ(unknown->decoded.size(), keyframes.size());

	// An avcC whose SPS allows fields
	auto interlaced = EncodeKeyframes({0, 1}, {}, cmn::MediaCodecId::H264, true);
	ASSERT_EQ(interlaced.size(), 2u);
	auto interlaced_avcc = std::static_pointer_cast<AVCDecoderConfigurationRecord>(MakeAvcRecord(interlaced[0]));
	H264SPS sps;
	ASSERT_EQ(interlaced_avcc->NumOfSPS(), 1);
	ASSERT_TRUE(H264Parser::ParseSPS(interlaced_avcc->GetSPSData(0)->GetDataAs<uint8_t>(), interlaced_avcc->GetSPSData(0)->GetLength(), sps));
	ASSERT_FALSE(sps.IsFrameMbsOnly());

	auto fields = DecodeSelected(framerates, keyframes, interlaced_avcc, nullptr, Wait::Never);
	EXPECT_EQ(fields->decoded.size(), keyframes.size());
}

TEST(AVCodecVideoDecoderKeyframeSelection, Vp8KeyframesAreSelectedUntilAHiddenOneAppears)
{
	auto keyframes = EncodeKeyframes(Periodic(2, 60), {}, cmn::MediaCodecId::Vp8);
	ASSERT_EQ(keyframes.size(), 31u);
	const std::vector<double> framerates = {static_cast<float>(0.1)};

	// select: the decoder may skip keyframes. hidden: the index of a key frame that gives no picture
	auto run = [&](const std::vector<Keyframe> &frames, bool select, int hidden) {
		auto pipeline = std::make_unique<Pipeline>(framerates, nullptr, cmn::MediaCodecId::Vp8);
		EXPECT_TRUE(pipeline->Start(select));
		bool waiting = select;
		for (size_t i = 0; i < frames.size(); i++)
		{
			// A hidden key frame gives no picture to wait for, and the decoder runs at the codec's pace after it
			pipeline->Feed(i, frames[i], waiting && static_cast<int>(i) != hidden);
			waiting = waiting && (static_cast<int>(i) != hidden);
		}
		pipeline->Finish(frames.size(), 0, waiting == false);
		return pipeline;
	};

	auto shown = run(keyframes, true, -1);
	EXPECT_EQ(shown->decoded, ExpectedDecoded(keyframes, framerates));

	// Keyframe 3 (6 s, the first of slot 1) is not shown (frame tag bit 4): the keyframe held for slot 0 is
	// decoded, and from there on every keyframe is, with the pictures decoding every keyframe gives
	auto hidden_keyframes = keyframes;
	hidden_keyframes[3].data[0] &= ~0x10;
	auto hidden = run(hidden_keyframes, true, 3);
	auto plain	= run(hidden_keyframes, false, 3);
	EXPECT_EQ(hidden->decoded.count(2), 1u);
	for (int i = 4; i < static_cast<int>(keyframes.size()); i++)
	{
		EXPECT_EQ(hidden->decoded.count(i), 1u) << i;
	}
	EXPECT_EQ(Sources(hidden->emitted[0]), Sources(plain->emitted[0]));
}

namespace
{
	// The decoder followed by the image output's TranscodeFilter (rescaler, FPS filter, rebuilds);
	// frames go from one to the other as OnDecodedFrame passes them on
	class FilterPipeline
	{
	public:
		explicit FilterPipeline(double framerate, std::shared_ptr<DecoderConfigurationRecord> avcc)
		{
			_input_track = InputTrack(avcc, cmn::MediaCodecId::H264);
			_input_track->SetFrameRateByConfig(kFps);
			_input_track->SetResolution(kSizes[0].width, kSizes[0].height);
			_input_track->SetColorspace(cmn::VideoPixelFormatId::YUV420P);

			_output_track = std::make_shared<MediaTrack>();
			_output_track->SetId(100);
			_output_track->SetMediaType(cmn::MediaType::Video);
			_output_track->SetCodecId(cmn::MediaCodecId::Jpeg);
			_output_track->SetCodecModuleId(cmn::MediaCodecModuleId::DEFAULT);
			_output_track->SetTimeBase(1, kTimescale);
			_output_track->SetFrameRateByConfig(framerate);
			_output_track->SetResolution(80, 48);
			_output_track->SetColorspace(cmn::VideoPixelFormatId::YUV420P);

			_framerate = static_cast<float>(_output_track->GetFrameRate());
		}

		~FilterPipeline()
		{
			if (_decoder != nullptr)
			{
				_decoder->Stop();
			}
			if (_filter != nullptr)
			{
				_filter->Stop();
			}
		}

		bool Start(bool select)
		{
			_filter = TranscodeFilter::Create(
				0, std::make_shared<info::Stream>(StreamSourceType::Ovt), _input_track, std::make_shared<info::Stream>(StreamSourceType::Ovt), _output_track,
				[this](TranscodeResult result, int32_t, std::shared_ptr<MediaFrame> frame) {
					if (result != TranscodeResult::DataReady || frame == nullptr)
					{
						return;
					}

					// Identifies the picture: the rescaled image, not a tag
					auto av_frame	  = static_cast<AVFrame *>(frame->GetPrivData());
					uint64_t checksum = 1469598103934665603ULL;
					for (int plane = 0; plane < 3; plane++)
					{
						int width  = (plane == 0) ? av_frame->width : av_frame->width / 2;
						int height = (plane == 0) ? av_frame->height : av_frame->height / 2;
						for (int y = 0; y < height; y++)
						{
							for (int x = 0; x < width; x++)
							{
								checksum = (checksum ^ av_frame->data[plane][y * av_frame->linesize[plane] + x]) * 1099511628211ULL;
							}
						}
					}

					std::lock_guard<std::mutex> lock(_mutex);
					images[SlotOf(frame->GetPts(), _framerate)] = checksum;
				});
			if (_filter == nullptr)
			{
				return false;
			}

			_decoder = std::make_shared<AVCodecVideoDecoder>(info::Stream(TestApplication::Get(), StreamSourceType::Transcoder), cmn::MediaCodecId::H264);
			_decoder->SetImageOutputs(select ? std::vector<KeyframeSelector::ImageOutput>{{_framerate, [this]() { return _filter->GetOutputFrameCount(); }}}
											 : std::vector<KeyframeSelector::ImageOutput>{});
			_decoder->SetCompleteHandler([this](TranscodeResult result, int32_t, std::shared_ptr<MediaFrame> frame) {
				if (frame != nullptr && (result == TranscodeResult::DataReady || result == TranscodeResult::FormatChanged))
				{
					_filter->SendBuffer(frame->CloneFrame());

					std::lock_guard<std::mutex> lock(_mutex);
					_handed_out.insert(frame->GetPts());
					_cv.notify_all();
				}
			});

			return _decoder->Configure(_input_track);
		}

		bool Feed(const Keyframe &keyframe, bool wait)
		{
			_decoder->SendBuffer(ToPacket(keyframe, cmn::MediaCodecId::H264));
			if (wait == false)
			{
				return true;
			}

			std::unique_lock<std::mutex> lock(_mutex);
			return _cv.wait_for(lock, std::chrono::seconds(5), [&]() { return _handed_out.count(keyframe.pts) > 0; });
		}

		// Drains the decoder, then waits until the filter has had nothing new for half a second
		void Finish()
		{
			_decoder->DrainAndStop();

			size_t seen = 0;
			for (int quiet = 0; quiet < 10;)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				std::lock_guard<std::mutex> lock(_mutex);
				quiet = (images.size() == seen) ? quiet + 1 : 0;
				seen  = images.size();
			}

			_filter->Stop();
		}

		// Slot -> checksum of the image
		std::map<int64_t, uint64_t> images;

	private:
		std::shared_ptr<MediaTrack> _input_track;
		std::shared_ptr<MediaTrack> _output_track;
		double _framerate = 0;
		std::shared_ptr<TranscodeFilter> _filter;
		std::shared_ptr<AVCodecVideoDecoder> _decoder;

		std::mutex _mutex;
		std::condition_variable _cv;
		std::set<int64_t> _handed_out;
	};

	std::map<int64_t, uint64_t> RunThroughTheFilter(double framerate, const std::vector<Keyframe> &keyframes, bool select)
	{
		FilterPipeline pipeline(framerate, MakeAvcRecord(keyframes[0]));
		EXPECT_TRUE(pipeline.Start(select));
		for (auto &keyframe : keyframes)
		{
			EXPECT_TRUE(pipeline.Feed(keyframe, select));
		}
		pipeline.Finish();

		return pipeline.images;
	}
}  // namespace

TEST(AVCodecVideoDecoderKeyframeSelection, AResolutionChangeGivesAtWorstTwoStaleSlotsThroughTheFilter)
{
	// 0.1 fps, a keyframe every 2 s; the picture size changes at 40 s, inside slot 4 (35-45 s)
	auto keyframes = EncodeKeyframes(Periodic(2, 100), {40});
	ASSERT_EQ(keyframes.size(), 51u);

	auto all  = RunThroughTheFilter(0.1, keyframes, false);
	auto held = RunThroughTheFilter(0.1, keyframes, true);

	ASSERT_GE(all.size(), 8u);
	std::vector<int64_t> stale;
	for (auto &[slot, checksum] : all)
	{
		auto it = held.find(slot);
		ASSERT_NE(it, held.end()) << "no image for slot " << slot;
		if (it->second != checksum)
		{
			stale.push_back(slot);
		}
	}
	EXPECT_EQ(held.size(), all.size());

	// Stale slots are within the change, none after it
	EXPECT_LE(stale.size(), 2u);
	for (auto slot : stale)
	{
		EXPECT_GE(slot, 4);
		EXPECT_LE(slot, 6);
	}
}

TEST(AVCodecVideoDecoderKeyframeSelection, FractionalRateBoundariesMatchTheUnchangedFilter)
{
	const auto encoded = EncodeKeyframes(Periodic(1, 17));
	ASSERT_EQ(encoded.size(), 18u);
	for (double rate : {0.1, 0.3, 0.7})
	{
		SCOPED_TRACE(rate);
		auto keyframes = encoded;
		size_t index = 0;
		for (int slot = 0; slot < 3; slot++)
		{
			// Near a one-hour timestamp, float narrowing moves these boundaries by several ticks.
			const auto boundary = std::llround((std::floor(3600 * rate) + slot + 0.5) / rate * kTimescale);
			for (int64_t offset : {-90000, -8, -4, -1, 0, 1})
			{
				auto &frame = keyframes[index++];
				const auto shift = boundary + offset - frame.pts;
				frame.pts += shift;
				frame.dts += shift;
			}
		}

		const auto all = RunThroughTheFilter(rate, keyframes, false);
		const auto selected = RunThroughTheFilter(rate, keyframes, true);
		ASSERT_GE(all.size(), 2u);
		EXPECT_EQ(selected, all);
	}
}

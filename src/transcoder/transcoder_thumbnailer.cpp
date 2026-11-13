//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "transcoder_thumbnailer.h"

#include <algorithm>

#include <modules/bitstream/h264/h264_decoder_configuration_record.h>
#include <modules/bitstream/h264/h264_parser.h>

#include "filter/filter_fps.h"
#include "filter/filter_lavfi_rescaler.h"
#include "transcoder_filter.h"
#include "transcoder_private.h"

namespace
{
	bool IsFrameCodedH264(const std::shared_ptr<MediaTrack> &track)
	{
		auto avcc = track->GetDecoderConfigurationRecordAs<AVCDecoderConfigurationRecord>();
		if ((avcc == nullptr) || (avcc->NumOfSPS() == 0))
		{
			return false;
		}

		for (int i = 0; i < avcc->NumOfSPS(); i++)
		{
			auto data = avcc->GetSPSData(i);
			H264SPS sps;
			if ((data == nullptr) || (H264Parser::ParseSPS(data->GetDataAs<uint8_t>(), data->GetLength(), sps) == false) || (sps.IsFrameMbsOnly() == false))
			{
				return false;
			}
		}

		return true;
	}

	// Whether every slice of the packet is an I slice. A unit that cannot be read is left to the decoder.
	bool IsIntraH264(const std::shared_ptr<const ov::Data> &data)
	{
		auto bytes = data->GetDataAs<uint8_t>();
		for (auto &nalu : H264Parser::FindNaluIndexes(bytes, data->GetLength()))
		{
			NalUnitBitstreamParser parser(bytes + nalu._payload_offset, nalu._payload_size);
			H264NalUnitHeader header;
			uint32_t first_mb_in_slice, slice_type;
			if ((H264Parser::ParseNalUnitHeader(parser, header) == false) ||
				((header.GetNalUnitType() != H264NalUnitType::IdrSlice) && (header.GetNalUnitType() != H264NalUnitType::NonIdrSlice)) ||
				(parser.ReadUEV(first_mb_in_slice) == false) || (parser.ReadUEV(slice_type) == false) || (slice_type > 9))
			{
				continue;
			}

			if (static_cast<H264SliceType>(slice_type % 5) != H264SliceType::I)
			{
				return false;
			}
		}

		return true;
	}
}  // namespace

bool TranscodeThumbnailer::IsSupported(const std::shared_ptr<MediaTrack> &input_track, const Outputs &outputs)
{
	if (input_track->IsKeyframeDecodeOnly() == false)
	{
		return false;
	}

	// FFmpeg's h264 decoder reads the parameter sets of a packet it skips (vp8 has none); hevc and dav1d do not.
	// Fields sent in two packets would be parted by the drain.
	auto codec_id = input_track->GetCodecId();
	if ((codec_id != cmn::MediaCodecId::Vp8) &&
		((codec_id != cmn::MediaCodecId::H264) || (IsFrameCodedH264(input_track) == false)))
	{
		return false;
	}

	for (auto &[input_stream, filter_input_track, output_stream, output_track, filter_id] : outputs)
	{
		if (output_track->GetSkipFramesByConfig() >= FilterFps::SkipFramesMin)
		{
			return false;
		}
	}

	return true;
}

std::shared_ptr<MediaPacket> TranscodeThumbnailer::GetPipelineStart(const std::shared_ptr<MediaPacket> &packet, const std::shared_ptr<const MediaTrack> &input_track)
{
	if ((packet->GetFlag() != MediaPacketFlag::Key) || (packet->GetDataLength() == 0))
	{
		return nullptr;
	}

	bool recovery_point = false;
	auto &data			= packet->GetData();
	auto bytes			= data->GetDataAs<uint8_t>();
	for (auto &nalu : H264Parser::FindNaluIndexes(bytes, data->GetLength()))
	{
		H264NalUnitHeader header;
		if ((H264Parser::ParseNalUnitHeader(bytes + nalu._payload_offset, nalu._payload_size, header) == true) &&
			(header.GetNalUnitType() == H264NalUnitType::IdrSlice))
		{
			return packet;
		}

		recovery_point |= H264Parser::IsRecoveryPointSei(bytes + nalu._payload_offset, nalu._payload_size);
	}

	auto avcc = input_track->GetDecoderConfigurationRecordAs<AVCDecoderConfigurationRecord>();
	if ((recovery_point == false) || (avcc == nullptr))
	{
		return nullptr;
	}

	// The media router puts the parameter sets in front of IDR pictures only
	auto start = std::make_shared<ov::Data>();
	for (int i = 0; i < avcc->NumOfSPS() + avcc->NumOfPPS(); i++)
	{
		auto unit = (i < avcc->NumOfSPS()) ? avcc->GetSPSData(i) : avcc->GetPPSData(i - avcc->NumOfSPS());
		if (unit != nullptr)
		{
			start->Append("\x00\x00\x00\x01", 4);
			start->Append(unit);
		}
	}
	start->Append(data);

	auto clone = packet->ClonePacket();
	clone->SetData(start);

	return clone;
}

int64_t TranscodeThumbnailer::GetSlot(int64_t pts, const cmn::Timebase &timebase, double framerate)
{
	return cmn::Rational::Rescale(
		pts,
		cmn::Rational(timebase.GetNum(), timebase.GetDen()),
		cmn::Rational::FromDouble(framerate, std::numeric_limits<int32_t>::max()).Invert());
}

std::shared_ptr<TranscodeThumbnailer> TranscodeThumbnailer::Create(
	MediaTrackId decoder_id,
	std::shared_ptr<MediaTrack> input_track,
	Outputs outputs,
	CompleteHandler decoded_handler,
	CompleteHandler filtered_handler)
{
	auto thumbnailer = std::make_shared<TranscodeThumbnailer>(
		decoder_id, std::move(input_track), std::move(outputs), std::move(decoded_handler), std::move(filtered_handler));

	return thumbnailer->Start() ? thumbnailer : nullptr;
}

TranscodeThumbnailer::TranscodeThumbnailer(
	MediaTrackId decoder_id,
	std::shared_ptr<MediaTrack> input_track,
	Outputs outputs,
	CompleteHandler decoded_handler,
	CompleteHandler filtered_handler)
	: _decoder_id(decoder_id),
	  _input_track(std::move(input_track)),
	  _outputs(std::move(outputs)),
	  _decoded_handler(std::move(decoded_handler)),
	  _filtered_handler(std::move(filtered_handler))
{
}

TranscodeThumbnailer::~TranscodeThumbnailer()
{
	{
		ov::LockGuard<ov::Mutex> lock(_mutex);
		_stop = true;
	}

	_condition.NotifyOne();
	if (_thread.joinable())
	{
		_thread.join();
	}
}

bool TranscodeThumbnailer::Start()
{
	auto codec_id	 = _input_track->GetCodecId();
	const char *name = (codec_id == cmn::MediaCodecId::H264) ? "h264" : (codec_id == cmn::MediaCodecId::Vp8) ? "vp8" : "";
	if ((_codec.AllocDecoderByName(name) == false) || (_codec.Open() == false))
	{
		logte("Could not open decoder(%s). %s", cmn::GetCodecIdString(codec_id), _codec.GetLastErrorString().CStr());
		return false;
	}

	// The rescalers read where the pictures of the track are; these are in host memory
	_input_track->SetCodecModuleId(cmn::MediaCodecModuleId::DEFAULT);

	try
	{
		_thread = std::thread(&TranscodeThumbnailer::ThreadLoop, this);
		pthread_setname_np(_thread.native_handle(), ov::String::FormatString("thumb_t%d", _input_track->GetId()).CStr());
	}
	catch (const std::system_error &e)
	{
		logte("Failed to start thumbnail thread: %s", e.what());
		return false;
	}

	return true;
}

bool TranscodeThumbnailer::SendBuffer(const std::shared_ptr<MediaPacket> &packet)
{
	{
		ov::LockGuard<ov::Mutex> lock(_mutex);
		if (_refused == true)
		{
			return false;
		}

		// The decoder of the pipeline drops the other packets of a keyframe-only track, and empty ones, as well
		if ((packet->GetFlag() != MediaPacketFlag::Key) || (packet->GetDataLength() == 0))
		{
			return true;
		}

		_keyframes.emplace_back(packet, std::chrono::steady_clock::now());
	}

	_condition.NotifyOne();

	return true;
}

std::optional<std::chrono::steady_clock::duration> TranscodeThumbnailer::GetShortestImageInterval() const
{
	std::optional<std::chrono::steady_clock::duration> shortest;
	for (auto &output : _outputs)
	{
		auto framerate = std::get<3>(output)->GetFrameRate();
		if (framerate > 0.0)
		{
			auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / framerate));
			shortest	  = shortest.has_value() ? std::min(*shortest, interval) : interval;
		}
	}

	return shortest;
}

std::shared_ptr<MediaFrame> TranscodeThumbnailer::Decode(const std::shared_ptr<MediaPacket> &keyframe, bool skip)
{
	// A skipped packet still gives the decoder its parameter sets, as the decoder of the pipeline has them
	_codec.Get()->skip_frame = skip ? AVDISCARD_ALL : AVDISCARD_DEFAULT;

	// The context has FFmpeg's default of one thread, so the packet's error comes back from this call.
	// Invalid data is a damaged keyframe: it gives no picture and is nothing to report.
	auto sent = _codec.SendPacket(keyframe);
	if ((sent != ffmpeg::CodecResult::Ok) && (sent != ffmpeg::CodecResult::InvalidData))
	{
		logte("Error occurred while decoding a keyframe. reason(%s)", _codec.GetLastErrorString().CStr());
		_decoded_handler(TranscodeResult::DataError, _decoder_id, nullptr);
	}

	// Drained, because the decoder holds the picture back when the stream declares reordering.
	// The flush lets it take packets again and keeps its parameter sets.
	_codec.SendEOS();

	std::shared_ptr<MediaFrame> picture;
	for (auto received = _codec.ReceiveFrame(); received.result == ffmpeg::CodecResult::Ok; received = _codec.ReceiveFrame())
	{
		picture = std::move(received.frame);
	}

	_codec.Flush();

	if (picture != nullptr)
	{
		picture->SetTrackId(_decoder_id);

		// A decoder that is never flushed gives out every picture after its first one. This one forgets
		// in the flush that it gave one, and would hold back keyframes without a recovery point.
		_codec.Get()->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
	}

	return picture;
}

void TranscodeThumbnailer::ThreadLoop()
{
	ov::logger::ThreadHelper thread_helper;

	while (true)
	{
		auto max_wait = GetShortestImageInterval();

		std::shared_ptr<MediaPacket> keyframe;
		{
			ov::LockGuard<ov::Mutex> lock(_mutex);

			_condition.Wait(lock, [this]() OV_REQUIRES(_mutex) -> bool {
				return _stop || (_keyframes.empty() == false);
			});

			if (_stop)
			{
				return;
			}

			// The thread does not keep up. A keyframe that waited longer than an image interval is not needed
			// when a newer one waits: an image is due for that one by then.
			auto now = std::chrono::steady_clock::now();
			while ((_keyframes.size() > 1) && max_wait.has_value() && ((now - _keyframes.front().second) > *max_wait))
			{
				if ((_dropped++ % 100) == 0)
				{
					logtw("Thumbnail thread of track %u is behind; a keyframe that waited too long is dropped (%zu so far)", _input_track->GetId(), _dropped);
				}

				_keyframes.pop_front();
			}

			keyframe = std::move(_keyframes.front().first);
			_keyframes.pop_front();
		}

		if ((_input_track->GetCodecId() == cmn::MediaCodecId::H264) && (IsIntraH264(keyframe->GetData()) == false))
		{
			// A publisher can flag any packet as a keyframe. The pictures of those that are not intra coded
			// come only from a decoder that decodes every one of them, so the stream takes the track back.
			ov::LockGuard<ov::Mutex> lock(_mutex);
			_refused = true;
			return;
		}

		auto slots = _slots;
		Outputs due;
		for (auto &output : _outputs)
		{
			auto &[input_stream, input_track, output_stream, output_track, filter_id] = output;

			auto slot = GetSlot(keyframe->GetPts(), input_track->GetTimeBase(), output_track->GetFrameRate());
			auto it	  = slots.find(filter_id);
			// Later, not other: when time steps back, the pipeline gives no frame either until it has passed
			// the last one, and an image encoder stops at a frame that is not later than its last one.
			if (it == slots.end() || it->second < slot)
			{
				slots[filter_id] = slot;
				due.push_back(output);
			}
		}

		auto picture = Decode(keyframe, due.empty());
		if (picture == nullptr)
		{
			continue;
		}

		_slots = std::move(slots);

		auto is_stale = [&](const std::shared_ptr<FilterLavfiRescaler> &rescaler) {
			return rescaler == nullptr ||
				   rescaler->GetState() != FilterBase::State::STARTED ||
				   TranscodeFilter::IsFormatChanged(rescaler, picture) == true;
		};

		// The stream takes a new format into the tracks, which a new rescaler reads
		bool changed = std::any_of(due.begin(), due.end(), [&](auto &output) { return is_stale(_rescalers[std::get<4>(output)]); });
		_decoded_handler(changed ? TranscodeResult::FormatChanged : TranscodeResult::DataReady, _decoder_id, picture);

		for (auto &[input_stream, input_track, output_stream, output_track, filter_id] : due)
		{
			auto &rescaler = _rescalers[filter_id];
			if (is_stale(rescaler) == true)
			{
				rescaler = std::make_shared<FilterLavfiRescaler>();
				rescaler->SetInputTrack(input_track);
				rescaler->SetOutputStreamInfo(output_stream);
				rescaler->SetOutputTrack(output_track);
				rescaler->Initialize();
			}

			// A rescaler in the error state takes no frame and gives none.
			rescaler->SendFrame(picture);
			_filtered_handler(TranscodeResult::DataReady, filter_id, rescaler->ReceiveFrame());
		}
	}
}

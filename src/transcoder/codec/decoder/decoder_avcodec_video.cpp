//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Keukhan
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "decoder_avcodec_video.h"

#include <modules/bitstream/h264/h264_decoder_configuration_record.h>
#include <modules/bitstream/h264/h264_parser.h>
#include <modules/bitstream/vp8/vp8_parser.h>

#include "../../transcoder_filter.h"
#include "../../transcoder_private.h"
#include "base/info/application.h"

// Both fields of an interlaced H.264 picture may come in separate packets, and a selected keyframe is
// decoded on its own. The avcC lists the SPS of the stream; none, or one that cannot be read, counts as interlaced.
static bool IsProgressiveAvc(const std::shared_ptr<MediaTrack> &track)
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

bool AVCodecVideoDecoder::Initialize()
{
	if (_analyzer.IsValid() == false)
	{
		if (_analyzer.Init(GetCodecID()) == false)
		{
			logte("Bitstream parser not found");
			return false;
		}
	}

	const char *decoder_name = nullptr;
	switch (GetCodecID())
	{
		case cmn::MediaCodecId::H264:
			decoder_name = "h264";
			break;
		case cmn::MediaCodecId::H265:
			decoder_name = "hevc";
			break;
		case cmn::MediaCodecId::Vp8:
			decoder_name = "vp8";
			break;
		case cmn::MediaCodecId::Av1:
			decoder_name = "libdav1d";
			break;
		default:
			logte("Unsupported codec for video decoder: %s", cmn::GetCodecIdString(GetCodecID()));
			return false;
	}

	if (_codec.AllocDecoderByName(decoder_name) == false)
	{
		// Fallback: if the preferred decoder is not available, try an alternative decoder.
		// For AV1, if libdav1d is not available, fall back to libaom-av1.
		// Note: This fallback mechanism is temporary and should be removed once libdav1d is guaranteed to be available.
		if ((GetCodecID() == cmn::MediaCodecId::Av1) && (_codec.AllocDecoderByName("libaom-av1") == true))
		{
			logtw("libdav1d decoder not found, falling back to libaom-av1. Rerun the prerequisites to rebuild FFmpeg with dav1d");
			decoder_name = "libaom-av1";
		}
		else
		{
			logte("Could not allocate decoder context for %s", cmn::GetCodecIdString(GetCodecID()));
			return false;
		}
	}

	_codec.SetTimeBase(GetTimebase());
	_codec.SetThreadCount(GetRefTrack()->GetThreadCount());
	_codec.SetThreadTypeFrame();

	// dav1d-only: keeps latency from growing as the thread count increases.
	// Keyframe-only decoding uses 1 so each keyframe comes out without waiting for the next
	if (::strcmp(decoder_name, "libdav1d") == 0)
	{
		_codec.SetOption("max_frame_delay", static_cast<int64_t>(GetRefTrack()->IsKeyframeDecodeOnly() ? 1 : 2));
	}

	if (_codec.Open() == false)
	{
		logte("Could not open decoder(%s). %s", cmn::GetCodecIdString(GetCodecID()), _codec.GetLastErrorString().CStr());
		return false;
	}

	_change_format = false;

	// A new session has no picture to stand in for a held keyframe yet
	_last_picture = nullptr;

	bool selectable = GetRefTrack()->IsKeyframeDecodeOnly() && KeyframeSelector::IsSupportedCodec(GetCodecID()) &&
					  ((GetCodecID() != cmn::MediaCodecId::H264) || IsProgressiveAvc(GetRefTrack()));
	_keyframe_selector.Configure(GetTimebase(), selectable ? GetImageOutputs() : std::vector<KeyframeSelector::ImageOutput>{},
								 static_cast<int64_t>(GetTimebase().GetTimescale()) * TranscodeFilter::kTimestampJumpLimitSeconds);

	return true;
}

bool AVCodecVideoDecoder::ReinitCodecIfNeed()
{
	if (_codec.GetWidth() != 0 &&
		_codec.GetHeight() != 0)
	{
		logti("[%s] Input frame resolution of the %u track has been changed. Size:%dx%d -> %dx%d",
			  _stream_info.GetUri().CStr(), GetRefTrack()->GetId(),
			  _codec.GetWidth(), _codec.GetHeight(),
			  _analyzer.GetWidth(), _analyzer.GetHeight());

		// Drain the frames still delayed inside the codec session and hand them
		// downstream before teardown, so the resolution change loses no frame.
		// This is the last chance to recover them, so a corrupt frame is skipped
		// instead of aborting (bounded against a codec stuck on invalid data)
		if (_codec.SendEOS() == ffmpeg::CodecResult::Ok)
		{
			for (int attempt = 0; attempt < kMaxDrainFrames; attempt++)
			{
				auto received = ReceiveFrame();
				if (received.result == TranscodeResult::DataReady || received.result == TranscodeResult::FormatChanged)
				{
					Complete(received.result, std::move(received.frame));
					continue;
				}
				if (received.result == TranscodeResult::NoData)
				{
					continue;
				}
				break;
			}
		}

		Uninitialize();

		if (Initialize() == false)
		{
			return false;
		}
	}

	return true;
}

std::shared_ptr<MediaPacket> AVCodecVideoDecoder::GetFramedPacket()
{
	auto obj = DequeueInput();
	if (obj.has_value() == false)
	{
		return nullptr;
	}

	auto media_packet = std::move(obj.value());

	const auto &data = media_packet->GetData();
	if (data == nullptr || data->GetLength() == 0)
	{
		logte("[%s] Could not analyze the bitstream packet", _stream_info.GetUri().CStr());
		return nullptr;
	}

	// Analyze the packet; if the input format (resolution) changed, reinit the codec.
	if (_analyzer.IsFormatChanged(media_packet) == true)
	{
		if (ReinitCodecIfNeed() == false)
		{
			return nullptr;
		}
	}

	return std::const_pointer_cast<MediaPacket>(media_packet);
}


DecodeResult AVCodecVideoDecoder::SendPacket(const std::shared_ptr<MediaPacket> &packet)
{
	bool drop_non_keyframe = (GetRefTrack()->IsKeyframeDecodeOnly() == true) && (packet->GetFlag() != MediaPacketFlag::Key);
	if (drop_non_keyframe == true)
	{
		return DecodeResult::NoOutput();
	}

	if (_keyframe_selector.IsEnabled() == true)
	{
		return SelectKeyframe(packet);
	}

	return SendToCodec(packet);
}

DecodeResult AVCodecVideoDecoder::SendToCodec(const std::shared_ptr<MediaPacket> &packet)
{
	auto result = _codec.SendPacket(packet);
	if (result == ffmpeg::CodecResult::InvalidData)
	{
		logtt("[%s] Invalid data while sending a packet for decoding. track(%u), pts(%" PRId64 ")",
			  _stream_info.GetUri().CStr(), GetRefTrack()->GetId(), packet->GetPts());

		auto empty_frame = MediaFrame::Create(cmn::MediaType::Video, packet->GetDts());

		return DecodeResult::InvalidData(std::move(empty_frame));
	}
	else if (result != ffmpeg::CodecResult::Ok)
	{
		logte("Error occurred while sending a packet for decoding. reason(%s)", _codec.GetLastErrorString().CStr());

		return DecodeResult::Error();
	}

	return DecodeResult::NoOutput();
}

DecodeResult AVCodecVideoDecoder::ReceiveFrame()
{
	auto received = _codec.ReceiveFrame();
	auto result = received.result;
	if (result == ffmpeg::CodecResult::Again || result == ffmpeg::CodecResult::Eof)
	{
		// Eof follows a drain (reinit or SendEOS())
		return DecodeResult::NoOutput();
	}
	else if (result == ffmpeg::CodecResult::InvalidData)
	{
		logtw("[%s] Invalid data while receiving a frame for decoding.", _stream_info.GetUri().CStr());
		return DecodeResult::InvalidData();
	}
	else if (result != ffmpeg::CodecResult::Ok)
	{
		logte("Error receiving a packet for decoding. reason(%s)", _codec.GetLastErrorString().CStr());
		return DecodeResult::Error();
	}

	bool format_changed = (_change_format == false);
	if (format_changed == true)
	{
		auto codec_info = _codec.GetCodecInfoString();
		logtd("[%s(%u)] Changed format. %s", _stream_info.GetUri().CStr(), _stream_info.GetId(), codec_info.CStr());
		_change_format = true;
	}

	auto decoded_frame = received.frame;
	if (decoded_frame == nullptr)
	{
		return DecodeResult::NoOutput();
	}

	// If the decoder did not provide a duration, calculate it from the frame rate
	const auto framerate = _codec.GetFrameRate();
	const auto timebase_expr = GetRefTrack()->GetTimeBase().GetExpr();
	if (decoded_frame->GetDuration() <= 0LL && framerate.GetExpr() > 0 && timebase_expr > 0)
	{
		decoded_frame->SetDuration((int64_t)((1.0 / framerate.GetExpr()) / timebase_expr));
	}

	if (_keyframe_selector.IsEnabled() == true)
	{
		_last_picture = decoded_frame;
	}

	return DecodeResult::Decoded(std::move(decoded_frame), format_changed);
}

DecodeResult AVCodecVideoDecoder::SelectKeyframe(const std::shared_ptr<MediaPacket> &keyframe)
{
	// A hidden VP8 key frame reaches no filter, so the keyframes the selector counts are not the ones the filters see
	if (GetCodecID() == cmn::MediaCodecId::Vp8)
	{
		const auto &data = keyframe->GetData();
		VP8Parser parser;
		if ((VP8Parser::Parse(data->GetDataAs<uint8_t>(), data->GetLength(), parser) == false) || (parser._show_frame == 0))
		{
			if (auto held = _keyframe_selector.Disable(); held != nullptr)
			{
				DecodeKeyframe(held);
			}

			return SendToCodec(keyframe);
		}
	}

	auto decision = _keyframe_selector.OnKeyframe(keyframe, _last_picture != nullptr);
	if (decision.decode_first != nullptr)
	{
		auto previous_picture = _last_picture;
		DecodeKeyframe(decision.decode_first);
		_keyframe_selector.OnHeldKeyframeDecoded(_last_picture != previous_picture);
	}

	if (decision.hold == true)
	{
		// The FPS filters of the image outputs advance on it as on the decoded keyframe
		auto stand_in = _last_picture->CloneFrame();
		stand_in->SetPts(keyframe->GetPts());

		return DecodeResult::Decoded(std::move(stand_in), false);
	}

	DecodeKeyframe(keyframe);

	return DecodeResult::NoOutput();
}

// A keyframe decoded after later ones were held has to reach the filters before the next stand-in,
// so it is not left to the codec's output delay (frame threads, reordering)
void AVCodecVideoDecoder::DecodeKeyframe(const std::shared_ptr<MediaPacket> &keyframe)
{
	auto sent = SendToCodec(keyframe);
	if (sent.result != TranscodeResult::Again)
	{
		Complete(sent.result, std::move(sent.frame));
		return;
	}

	FlushCodec();
	_codec.Flush();
}

bool AVCodecVideoDecoder::SendEOS()
{
	return _codec.SendEOS() == ffmpeg::CodecResult::Ok;
}

void AVCodecVideoDecoder::Uninitialize()
{
	_codec.Flush();
	_codec.Reset();
}

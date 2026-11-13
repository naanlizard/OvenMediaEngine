//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <modules/ffmpeg/ffmpeg_codec.h>

#include <chrono>
#include <deque>
#include <optional>

#include "codec/codec_base.h"
#include "transcoder_composite.h"

class FilterLavfiRescaler;

class TranscodeThumbnailer
{
public:
	typedef std::function<void(TranscodeResult, MediaTrackId, std::shared_ptr<MediaFrame>)> CompleteHandler;
	typedef std::vector<CompositeMap::StreamTrackPairNo> Outputs;

	static bool IsSupported(const std::shared_ptr<MediaTrack> &input_track, const Outputs &outputs);
	// After a hand-back: the packet the decoder of the pipeline can start at without building a picture on
	// references it never had, an H.264 IDR picture or recovery point; nullptr for any other packet
	static std::shared_ptr<MediaPacket> GetPipelineStart(const std::shared_ptr<MediaPacket> &packet, const std::shared_ptr<const MediaTrack> &input_track);
	static int64_t GetSlot(int64_t pts, const cmn::Timebase &timebase, double framerate);

	static std::shared_ptr<TranscodeThumbnailer> Create(
		MediaTrackId decoder_id,
		std::shared_ptr<MediaTrack> input_track,
		Outputs outputs,
		CompleteHandler decoded_handler,
		CompleteHandler filtered_handler);

	TranscodeThumbnailer(
		MediaTrackId decoder_id,
		std::shared_ptr<MediaTrack> input_track,
		Outputs outputs,
		CompleteHandler decoded_handler,
		CompleteHandler filtered_handler);
	~TranscodeThumbnailer();

	// False once the key packets of the track turned out not to be intra coded; the packet is then not taken
	bool SendBuffer(const std::shared_ptr<MediaPacket> &packet);

private:
	bool Start();
	void ThreadLoop();
	std::optional<std::chrono::steady_clock::duration> GetShortestImageInterval() const;
	std::shared_ptr<MediaFrame> Decode(const std::shared_ptr<MediaPacket> &keyframe, bool skip);

	MediaTrackId _decoder_id;
	std::shared_ptr<MediaTrack> _input_track;
	Outputs _outputs;
	CompleteHandler _decoded_handler;
	CompleteHandler _filtered_handler;

	// Open for the life of the track and given every key packet in order, as the decoder of the pipeline is
	ffmpeg::FFmpegCodec _codec;
	// [FILTER_ID, SLOT of the last image]
	std::map<MediaTrackId, int64_t> _slots;
	// [FILTER_ID, RESCALER]
	std::map<MediaTrackId, std::shared_ptr<FilterLavfiRescaler>> _rescalers;

	ov::Mutex _mutex;
	ov::ConditionVariable _condition;
	// [KEYFRAME, TIME it was taken]
	std::deque<std::pair<std::shared_ptr<MediaPacket>, std::chrono::steady_clock::time_point>> _keyframes OV_GUARDED_BY(_mutex);
	size_t _dropped = 0;
	bool _refused OV_GUARDED_BY(_mutex) = false;
	bool _stop OV_GUARDED_BY(_mutex) = false;

	std::thread _thread;
};

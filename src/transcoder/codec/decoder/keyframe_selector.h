//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/mediarouter/media_buffer.h>
#include <base/mediarouter/media_type.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// Keyframe-only decoding for image outputs. The FPS filter of an image output keeps only the
// last frame of each output slot, so a keyframe is held until the next one shows whether it is
// that frame, and is decoded only if it is the last of its slot for some output.
//
// Selection restarts when the keyframes' timestamps jump or step back, when an output has had
// two slot boundaries and no image, and when a held keyframe decoded as the last of its slot gives
// no picture: the held keyframe is dropped, the current keyframe is decoded as the first of a new
// run, and holding resumes after it. After kMaxRestarts restarts in a row that an output has
// caused without producing an image, or kMaxRestarts such decodes in a row without a picture, the
// stream is decoded as before.
class KeyframeSelector
{
public:
	struct ImageOutput
	{
		// The rate of the output's FPS filter
		double framerate = 0.0;
		// How many images the output has produced so far
		std::function<uint64_t()> images;
	};

	struct Decision
	{
		// The held keyframe, still needed: decode it before anything else
		std::shared_ptr<MediaPacket> decode_first;
		// Hold the new keyframe instead of decoding it now
		bool hold = false;
	};

	static constexpr int kMaxRestarts = 3;

	// Starts a new run; only Disable() is kept. max_gap: a keyframe more than this after the previous
	// one, or before it, restarts selection.
	void Configure(const cmn::Timebase &timebase, std::vector<ImageOutput> outputs, int64_t max_gap);

	bool IsEnabled() const;

	// can_hold: a decoded picture exists to stand in for the keyframe while it is held.
	Decision OnKeyframe(const std::shared_ptr<MediaPacket> &keyframe, bool can_hold);

	// The decoder reports each decode of the keyframe OnKeyframe() gave back as decode_first. Without a
	// picture, the next keyframe restarts selection: its stand-ins would keep the images coming.
	void OnHeldKeyframeDecoded(bool produced_picture);

	// Selection is over for the life of the stream. Returns the held keyframe, still to be decoded
	std::shared_ptr<MediaPacket> Disable();

	// H.264 and VP8; other decoders keep their own pace
	static bool IsSupportedCodec(cmn::MediaCodecId codec_id);

private:
	struct Output
	{
		std::function<uint64_t()> images;
		size_t clock = 0;
		uint64_t seen = 0;
		// Slot boundaries crossed by keyframes since the output last produced an image
		int64_t boundaries = 0;
		// Restarts in a row it has caused without producing an image
		int restarts = 0;
	};

	void Restart();

	cmn::Rational _input_timebase;
	// One per distinct output rate
	std::vector<cmn::Rational> _slot_timebases;
	std::vector<Output> _outputs;
	int64_t _max_gap = 0;

	std::shared_ptr<MediaPacket> _held;
	std::optional<int64_t> _previous_pts;
	std::vector<int64_t> _previous_slots;
	int _failed_decodes = 0;
	bool _decode_failed = false;
	bool _disabled		= false;
};

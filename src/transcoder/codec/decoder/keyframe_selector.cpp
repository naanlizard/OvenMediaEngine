//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "keyframe_selector.h"

#include <algorithm>
#include <utility>
#include <limits>


void KeyframeSelector::Configure(const cmn::Timebase &timebase, std::vector<ImageOutput> outputs, int64_t max_gap)
{
	_input_timebase = cmn::Rational(timebase.GetNum(), timebase.GetDen());
	_max_gap		= max_gap;

	std::vector<double> framerates;
	_slot_timebases.clear();
	_outputs.clear();
	for (auto &output : outputs)
	{
		auto it = std::find(framerates.begin(), framerates.end(), output.framerate);
		if (it == framerates.end())
		{
			framerates.push_back(output.framerate);
			// Match the FPS filter's slot rounding without changing its interface.
			_slot_timebases.push_back(cmn::Rational::FromDouble(output.framerate, std::numeric_limits<int32_t>::max()).Invert());
			it = std::prev(framerates.end());
		}

		_outputs.push_back({std::move(output.images), static_cast<size_t>(it - framerates.begin()), 0, 0, 0});
	}

	_failed_decodes = 0;
	Restart();
}

bool KeyframeSelector::IsEnabled() const
{
	return (_outputs.empty() == false) && (_disabled == false);
}

void KeyframeSelector::Restart()
{
	_held		   = nullptr;
	_decode_failed = false;
	_previous_pts.reset();
}

KeyframeSelector::Decision KeyframeSelector::OnKeyframe(const std::shared_ptr<MediaPacket> &keyframe, bool can_hold)
{
	Decision decision;
	const int64_t pts = keyframe->GetPts();

	for (auto &output : _outputs)
	{
		if (auto images = output.images(); images != output.seen)
		{
			output.seen		  = images;
			output.boundaries = 0;
			output.restarts	  = 0;
		}
	}

	// TranscodeFilter rebuilds the image filters on a timestamp jump, and they drop frames stamped behind their output position
	const bool discontinuous = _previous_pts.has_value() && ((pts < _previous_pts.value()) || (pts - _previous_pts.value() > _max_gap));

	// An output with two slot boundaries crossed and no image since has stopped producing. Each output has its own
	// restart count and boundaries, so one that keeps producing, or restarts that others caused, do not hide it
	bool restart = discontinuous || _decode_failed;
	bool give_up = (_failed_decodes > kMaxRestarts);
	for (auto &output : _outputs)
	{
		if (discontinuous || (output.boundaries >= 2))
		{
			restart			  = true;
			give_up			  = give_up || (output.restarts >= kMaxRestarts);
			output.boundaries = 0;
			output.restarts++;
		}
	}

	if (restart == true)
	{
		if (give_up == true)
		{
			decision.decode_first = Disable();
			return decision;
		}

		Restart();
	}

	std::vector<int64_t> slots;
	for (const auto &slot_timebase : _slot_timebases)
	{
		slots.push_back(cmn::Rational::Rescale(pts, _input_timebase, slot_timebase));
	}

	if (_previous_pts.has_value() == true)
	{
		bool skips_slot = false;
		bool same_slot	= true;
		for (size_t i = 0; i < slots.size(); i++)
		{
			skips_slot = skips_slot || (slots[i] - _previous_slots[i] > 1);
			same_slot  = same_slot && (slots[i] == _previous_slots[i]);
		}

		// A keyframe past a whole slot is decoded at once: the filter fills the slots it skips from the frames around it
		decision.hold = can_hold && (skips_slot == false);

		// The held keyframe is dropped only where the FPS filters would drop it: the new one takes its slot at every rate
		if ((_held != nullptr) && ((decision.hold == false) || (same_slot == false)))
		{
			decision.decode_first = _held;
		}

		for (auto &output : _outputs)
		{
			output.boundaries += std::max<int64_t>(0, slots[output.clock] - _previous_slots[output.clock]);
		}
	}

	_held			= decision.hold ? keyframe : nullptr;
	_previous_pts	= pts;
	_previous_slots = std::move(slots);

	return decision;
}

void KeyframeSelector::OnHeldKeyframeDecoded(bool produced_picture)
{
	_failed_decodes = produced_picture ? 0 : _failed_decodes + 1;
	_decode_failed	= _decode_failed || (produced_picture == false);
}

std::shared_ptr<MediaPacket> KeyframeSelector::Disable()
{
	_disabled = true;

	return std::exchange(_held, nullptr);
}

bool KeyframeSelector::IsSupportedCodec(cmn::MediaCodecId codec_id)
{
	return (codec_id == cmn::MediaCodecId::H264) || (codec_id == cmn::MediaCodecId::Vp8);
}

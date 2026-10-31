//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/mediarouter/media_type.h>
#include <base/ovlibrary/ovlibrary.h>

#include <chrono>
#include <ctime>

// Per-frame wall and thread-CPU time for image tracks, logged under the "ThumbStat" tag at debug level.
// Disabled, it reads no clock.
class ThumbStatTimer
{
public:
	explicit ThumbStatTimer(bool enabled)
		: _enabled(enabled)
	{
		if (_enabled)
		{
			::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &_cpu0);
			_wall0 = std::chrono::steady_clock::now();
		}
	}

	bool IsEnabled() const
	{
		return _enabled;
	}

	int64_t WallElapsedUs() const
	{
		if (_enabled == false)
		{
			return 0;
		}

		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _wall0).count();
	}

	int64_t CpuElapsedUs() const
	{
		if (_enabled == false)
		{
			return 0;
		}

		timespec cpu1 = {};
		::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
		return ((int64_t)cpu1.tv_sec - _cpu0.tv_sec) * 1000000 + ((int64_t)cpu1.tv_nsec - _cpu0.tv_nsec) / 1000;
	}

	// wall_us and cpu_us: the caller's own totals for work that spans several calls
	void Emit(const char *stage, cmn::MediaCodecId codec, const ov::String &variant, int64_t wall_us = -1, int64_t cpu_us = -1) const
	{
		if (_enabled == false)
		{
			return;
		}

		if (cpu_us < 0)
		{
			cpu_us = CpuElapsedUs();
		}

		if (wall_us < 0)
		{
			wall_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _wall0).count();
		}

		logd("ThumbStat", "[THUMBSTAT] stage=%s codec=%s variant=%s wall_us=%lld cpu_us=%lld",
			 stage, cmn::GetCodecIdString(codec), variant.CStr(), (long long)wall_us, (long long)cpu_us);
	}

private:
	bool _enabled;
	timespec _cpu0 = {};
	std::chrono::steady_clock::time_point _wall0;
};

//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

#define ORIGIN_MAP_STORE_RECONNECT_BACKOFF_MIN_MS 1000
// Below the key TTL, so an origin re-SETs its keys within one TTL of Redis coming back.
#define ORIGIN_MAP_STORE_RECONNECT_BACKOFF_MAX_MS 5000

class OriginMapReconnectBackoff
{
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;

	explicit OriginMapReconnectBackoff(Clock clock = std::chrono::steady_clock::now)
		: _clock(std::move(clock))
	{
	}

	bool IsBackingOff() const
	{
		return (_delay.count() > 0) && (_clock() < _retry_at);
	}

	void OnFailure()
	{
		_delay = (_delay.count() == 0)
					 ? std::chrono::milliseconds(ORIGIN_MAP_STORE_RECONNECT_BACKOFF_MIN_MS)
					 : std::min(_delay * 2, std::chrono::milliseconds(ORIGIN_MAP_STORE_RECONNECT_BACKOFF_MAX_MS));
		_retry_at = _clock() + _delay;
	}

	void Reset()
	{
		_delay = std::chrono::milliseconds::zero();
	}

	std::chrono::milliseconds GetDelay() const
	{
		return _delay;
	}

private:
	Clock _clock;
	std::chrono::milliseconds _delay{0};
	std::chrono::steady_clock::time_point _retry_at;
};

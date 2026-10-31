//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2022 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/common_types.h>
#include <base/ovlibrary/ovlibrary.h>
#include <base/ovlibrary/delay_queue.h>
#include <hiredis/hiredis.h>

#include <atomic>

#include "reconnect_backoff.h"

// redis key expire time (sec)
#define ORIGIN_MAP_STORE_KEY_EXPIRE_TIME 10

// A half-open peer otherwise blocks this thread forever; the 500ms timer that re-registers streams runs on it.
#define ORIGIN_MAP_STORE_CONNECT_TIMEOUT_MS 3000
#define ORIGIN_MAP_STORE_COMMAND_TIMEOUT_MS 3000

static_assert(ORIGIN_MAP_STORE_RECONNECT_BACKOFF_MAX_MS < ORIGIN_MAP_STORE_KEY_EXPIRE_TIME * 1000);

// If Origins-Edges cluster uses OriginMapStore, app/stream must be unique in the cluster.
class OriginMapClient
{
public:
	// redis_host: redis server host (ex: 192.168.0.160:6379)
	// redis_password: redis server password (ex: password!@#)
	OriginMapClient(const ov::String &redis_host, const ov::String &redis_password);
	~OriginMapClient();

	// Non-blocking request to register origin host
	bool RequestRegister(const ov::String &app_stream_name, const ov::String &origin_host); 
	bool RequestUnregister(const ov::String &app_stream_name);

	CommonErrorCode GetOrigin(const ov::String &app_stream_name, ov::String &origin_host);

private:
	// if return false, it means that the app_stream_name is already registered from other origin server
	// app_stream_name : app/stream name (ex: app/stream)
	// origin_host : OVT provided origin host info (ex: 192.168.0.160:9000)
	bool Register(const ov::String &app_stream_name, const ov::String &origin_host);
	// taken: set when another origin holds the key
	bool Update(const ov::String &app_stream_name, const ov::String &origin_host, bool *taken = nullptr);
	bool Unregister(const ov::String &app_stream_name, const ov::String &origin_host);

	bool ConnectRedis();
	struct timeval TimeoutWithin(int timeout_ms) const;

	bool NofifyStreamsAlive();
	bool RetryRegister();
	bool ProcessRequests();

	enum class Replay
	{
		Allowed,
		Forbidden
	};
	std::tuple<int, ov::String> CommandToRedis(Replay replay, const char *format, ...);

	struct Request
	{
		ov::String key;
		ov::String origin;
		bool remove;
		uint64_t sequence;
	};

	ov::String _redis_ip;
	uint16_t _redis_port;
	ov::String _redis_password;

	ov::DelayQueue _update_timer{"OMapC"};

	// Only the timer accesses these maps until the destructor joins it.
	std::map<ov::String, ov::String> _origin_map;
	std::map<ov::String, ov::String> _origin_map_candidates;

	// Preserve caller order, including unregister/register pairs with the same URL.
	std::deque<Request> _requests;
	std::map<ov::String, ov::String> _requested_origins;
	uint64_t _next_request_sequence = 0;
	std::mutex _requests_mutex;
	std::atomic<bool> _stopping{false};

	redisContext *_redis_context = nullptr;
	// Set for the destructor's cleanup: socket timeouts are cut to end by then
	std::chrono::steady_clock::time_point _deadline{};
	OriginMapReconnectBackoff _reconnect_backoff;
	std::mutex _redis_context_mutex;
};
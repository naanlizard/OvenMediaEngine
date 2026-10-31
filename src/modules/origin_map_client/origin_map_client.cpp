//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2022 AirenSoft. All rights reserved.
//
//==============================================================================

#include "origin_map_client.h"

#include <algorithm>
#include <iterator>
#include <set>

#define OV_LOG_TAG "OriginMapClient"

namespace
{
	constexpr const char *kConnectFailedReply = "Failed to connect redis server";
	constexpr const char *kBackingOffReply = "Waiting to reconnect to redis server";

	// The stored OVT URL is the ownership identity. An absent key can be restored
	// after Redis recovery, but an origin that took over must never be overwritten.
	constexpr const char *kRefreshOrigin =
		"local owner = redis.call('GET', KEYS[1]); "
		"if not owner or owner == ARGV[1] then "
		"redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2]); return 1; end; return 0";
	constexpr const char *kRemoveOrigin =
		"if redis.call('GET', KEYS[1]) == ARGV[1] then "
		"return redis.call('DEL', KEYS[1]); end; return 0";

	bool IsUnreachableReply(int reply_type, const ov::String &reply_str)
	{
		return (reply_type == REDIS_ERR) && ((reply_str == kConnectFailedReply) || (reply_str == kBackingOffReply));
	}
}  // namespace

// ConnectRedis logs each failed connect once, with its backoff window; callers repeat that failure at debug level only.
#define logte_unless_unreachable(reply_type, reply_str, format, ...) \
	ov_log_internal(IsUnreachableReply(reply_type, reply_str) ? OVLogLevelDebug : OVLogLevelError, OV_LOG_TAG, __FILE__, __LINE__, __PRETTY_FUNCTION__, format, ##__VA_ARGS__)

OriginMapClient::OriginMapClient(const ov::String &redis_host, const ov::String &redis_password)
{
	// Parse ip:port 
	auto ip_port = redis_host.Split(":");
	if (ip_port.size() != 2)
	{
		return;
	}

	_redis_ip = ip_port[0];
	_redis_port = ov::Converter::ToUInt16(ip_port[1]);
	_redis_password = redis_password;

	_update_timer.Push(
		[this](void *paramter) -> ov::DelayQueueAction {
			ProcessRequests();
			RetryRegister();
			NofifyStreamsAlive();
			return ov::DelayQueueAction::Repeat;
		},
		500);
	_update_timer.Start();
}

OriginMapClient::~OriginMapClient()
{
	// Declared before the state its callback uses, so the timer would otherwise stop after that state is gone,
	// and a callback still running could recreate a key removed below
	_stopping = true;
	_update_timer.Stop();

	// One attempt even inside a backoff window; the keys otherwise stay until their TTL
	_reconnect_backoff.Reset();

	std::set<std::pair<ov::String, ov::String>> removals;
	for (const auto &entry : _origin_map)
	{
		removals.emplace(entry);
	}
	// A registration whose reply was lost may have reached Redis.
	for (const auto &entry : _origin_map_candidates)
	{
		removals.emplace(entry);
	}
	for (const auto &request : _requests)
	{
		if (request.remove)
		{
			removals.emplace(request.key, request.origin);
		}
	}
	// Bounded, as the last reference can be dropped under the orchestrator's virtual host lock
	{
		std::lock_guard<std::mutex> lock(_redis_context_mutex);
		_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ORIGIN_MAP_STORE_COMMAND_TIMEOUT_MS);
	}
	for (const auto &[key, origin] : removals)
	{
		if (Unregister(key, origin) == false)
		{
			// The remaining keys expire with their TTL
			break;
		}
	}
	if (_redis_context != nullptr)
	{
		redisFree(_redis_context);
		_redis_context = nullptr;
	}
}

bool OriginMapClient::NofifyStreamsAlive()
{
	for (const auto &[key, value] : _origin_map)
	{
		if (_stopping)
		{
			break;
		}
		Update(key, value);
	}
	return true;
}

bool OriginMapClient::RetryRegister()
{
	for (auto it = _origin_map_candidates.begin(); it != _origin_map_candidates.end();)
	{
		if (_stopping)
		{
			break;
		}
		if (Register(it->first, it->second))
		{
			it = _origin_map_candidates.erase(it);
		}
		else
		{
			++it;
		}
	}
	return true;
}

bool OriginMapClient::ProcessRequests()
{
	// Only what was queued before this pass, so requests arriving during it cannot hold off the refresh
	uint64_t watermark = 0;
	{
		std::lock_guard<std::mutex> lock(_requests_mutex);
		watermark = _next_request_sequence;
	}

	while (_stopping == false)
	{
		Request request;
		{
			std::lock_guard<std::mutex> lock(_requests_mutex);
			if (_requests.empty() || (_requests.front().sequence >= watermark))
			{
				break;
			}
			request = std::move(_requests.front());
			_requests.pop_front();
		}
		if (request.remove)
		{
			auto candidate = _origin_map_candidates.find(request.key);
			if (candidate != _origin_map_candidates.end() && candidate->second == request.origin)
			{
				_origin_map_candidates.erase(candidate);
			}
			auto owned = _origin_map.find(request.key);
			if (owned != _origin_map.end() && owned->second == request.origin)
			{
				_origin_map.erase(owned);
			}
			Unregister(request.key, request.origin);
		}
		else
		{
			// A new URL for the key retires the one registered, or possibly registered, before; nothing would unregister it otherwise
			auto owned = _origin_map.find(request.key);
			if ((owned != _origin_map.end()) && (owned->second != request.origin))
			{
				auto previous_origin = owned->second;
				_origin_map.erase(owned);
				Unregister(request.key, previous_origin);
			}
			auto candidate = _origin_map_candidates.find(request.key);
			if ((candidate != _origin_map_candidates.end()) && (candidate->second != request.origin))
			{
				auto previous_origin = candidate->second;
				_origin_map_candidates.erase(candidate);
				Unregister(request.key, previous_origin);
			}
			_origin_map_candidates[request.key] = request.origin;
		}
	}
	return true;
}

bool OriginMapClient::RequestRegister(const ov::String &app_stream_name, const ov::String &origin_host)
{
	std::lock_guard<std::mutex> lock(_requests_mutex);
	_requested_origins[app_stream_name] = origin_host;

	// A stream back before its queued removal ran keeps its Redis entry rather than losing it until re-registration
	auto last = std::find_if(_requests.rbegin(), _requests.rend(), [&](const Request &request) { return request.key == app_stream_name; });
	if (last != _requests.rend() && last->remove && last->origin == origin_host)
	{
		_requests.erase(std::next(last).base());
		return true;
	}

	_requests.push_back({app_stream_name, origin_host, false, _next_request_sequence++});
	return true;
}

bool OriginMapClient::RequestUnregister(const ov::String &app_stream_name)
{
	std::lock_guard<std::mutex> lock(_requests_mutex);
	auto requested = _requested_origins.find(app_stream_name);
	if (requested != _requested_origins.end())
	{
		_requests.push_back({app_stream_name, requested->second, true, _next_request_sequence++});
		_requested_origins.erase(requested);
	}
	return true;
}

bool OriginMapClient::Register(const ov::String &app_stream_name, const ov::String &origin_host)
{
	bool taken = false;
	if (!Update(app_stream_name, origin_host, &taken))
	{
		if (taken)
		{
			logte("<%s> stream is already registered with a different origin host", app_stream_name.CStr());
		}
		return false;
	}
	_origin_map[app_stream_name] = origin_host;
	logti("OriginMapStore: <%s> stream is registered with origin host : %s", app_stream_name.CStr(), origin_host.CStr());
	return true;
}

bool OriginMapClient::Update(const ov::String &app_stream_name, const ov::String &origin_host, bool *taken)
{
	auto [reply_type, reply_str] = CommandToRedis(Replay::Allowed, "EVAL %s 1 %s %s %d", kRefreshOrigin, app_stream_name.CStr(), origin_host.CStr(), ORIGIN_MAP_STORE_KEY_EXPIRE_TIME);
	if (reply_type == REDIS_REPLY_INTEGER)
	{
		if (taken != nullptr)
		{
			*taken = (reply_str != "1");
		}
		return reply_str == "1";
	}
	logte_unless_unreachable(reply_type, reply_str, "Failed to update origin host to redis : %s:%d (err(%d):%s)", _redis_ip.CStr(), _redis_port, reply_type, reply_str.CStr());
	return false;
}

bool OriginMapClient::Unregister(const ov::String &app_stream_name, const ov::String &origin_host)
{
	// Keep deletion non-replayable: identical URLs do not distinguish generations.
	auto [reply_type, reply_str] = CommandToRedis(Replay::Forbidden, "EVAL %s 1 %s %s", kRemoveOrigin, app_stream_name.CStr(), origin_host.CStr());
	if (reply_type != REDIS_REPLY_INTEGER)
	{
		logte_unless_unreachable(reply_type, reply_str, "Failed to delete origin host from redis : %s:%d (err(%d):%s)", _redis_ip.CStr(), _redis_port, reply_type, reply_str.CStr());
		return false;
	}
	logti("OriginMapStore: <%s> stream is unregistered.", app_stream_name.CStr());
	return true;
}

CommonErrorCode OriginMapClient::GetOrigin(const ov::String &app_stream_name, ov::String &origin_host)
{
	auto [reply_type, reply_str] = CommandToRedis(Replay::Allowed, "GET %s", app_stream_name.CStr());
	if (reply_type == REDIS_ERR || reply_type == REDIS_REPLY_ERROR)
	{
		logte_unless_unreachable(reply_type, reply_str, "Failed to get origin host from redis : %s:%d (err(%d):%s)", _redis_ip.CStr(), _redis_port, reply_type, reply_str.CStr());
		return CommonErrorCode::ERROR;
	}
	else if (reply_type == REDIS_REPLY_NIL)
	{
		return CommonErrorCode::NOT_FOUND;
	}

	origin_host = reply_str;

	return CommonErrorCode::SUCCESS;
}

struct timeval OriginMapClient::TimeoutWithin(int timeout_ms) const
{
	int64_t ms = timeout_ms;
	if (_deadline != std::chrono::steady_clock::time_point{})
	{
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(_deadline - std::chrono::steady_clock::now()).count();
		ms = std::clamp<int64_t>(remaining, 1, ms);
	}

	return { static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000) };
}

// Caller holds _redis_context_mutex.
bool OriginMapClient::ConnectRedis()
{
	if (_redis_context != nullptr)
	{
		return true;
	}

	auto connect_timeout = TimeoutWithin(ORIGIN_MAP_STORE_CONNECT_TIMEOUT_MS);

	// connect to redis server
	_redis_context = redisConnectWithTimeout(_redis_ip.CStr(), _redis_port, connect_timeout);
	if (_redis_context == nullptr || _redis_context->err)
	{
		_reconnect_backoff.OnFailure();

		logte("Failed to connect to redis server. ip: %s, port: %d, err: %s, next attempt in %lld ms", _redis_ip.CStr(), _redis_port, _redis_context!=nullptr?_redis_context->errstr:"cannot allocate context", static_cast<long long>(_reconnect_backoff.GetDelay().count()));

		if (_redis_context != nullptr)
		{
			redisFree(_redis_context);
			_redis_context = nullptr;
		}

		return false;
	}

	redisSetTimeout(_redis_context, TimeoutWithin(ORIGIN_MAP_STORE_COMMAND_TIMEOUT_MS));
	redisEnableKeepAlive(_redis_context);

	// Auth
	if (_redis_password.IsEmpty() == false)
	{
		redisReply *reply = (redisReply *)redisCommand(_redis_context, "AUTH %s", _redis_password.CStr());
		if (reply == nullptr || reply->type == REDIS_REPLY_ERROR)
		{
			_reconnect_backoff.OnFailure();

			logte("Failed to auth to redis server. ip: %s, port: %d, err: %s, next attempt in %lld ms", _redis_ip.CStr(), _redis_port, reply!=nullptr?reply->str:"nil", static_cast<long long>(_reconnect_backoff.GetDelay().count()));

			if (reply != nullptr)
			{
				freeReplyObject(reply);
			}

			redisFree(_redis_context);
			_redis_context = nullptr;

			return false;
		}

		freeReplyObject(reply);
	}

	return true;
}

std::tuple<int, ov::String> OriginMapClient::CommandToRedis(Replay replay, const char *format, ...)
{
	std::lock_guard<std::mutex> lock(_redis_context_mutex);

	if (_reconnect_backoff.IsBackingOff())
	{
		return { REDIS_ERR, kBackingOffReply };
	}

	// A reused connection may have been dropped while idle, so its command gets one retry on a new connection.
	int attempts = ((replay == Replay::Allowed) && (_redis_context != nullptr)) ? 2 : 1;
	redisReply *reply = nullptr;

	while ((reply == nullptr) && (attempts-- > 0))
	{
		bool bounded = (_deadline != std::chrono::steady_clock::time_point{});
		if (bounded && (std::chrono::steady_clock::now() >= _deadline))
		{
			return { REDIS_ERR, "Out of time for redis" };
		}

		if (ConnectRedis() == false)
		{
			return { REDIS_ERR, kConnectFailedReply };
		}

		if (bounded)
		{
			redisSetTimeout(_redis_context, TimeoutWithin(ORIGIN_MAP_STORE_COMMAND_TIMEOUT_MS));
		}

		va_list args;
		va_start(args, format);
		reply = (redisReply *)redisvCommand(_redis_context, format, args);
		va_end(args);

		if (reply == nullptr)
		{
			logte("Failed to execute command to redis : %s:%d (err:%s)", _redis_ip.CStr(), _redis_port, _redis_context->errstr);

			redisFree(_redis_context);
			_redis_context = nullptr;
		}
	}

	if (reply == nullptr)
	{
		// A server that accepts connections but does not answer gets the same backoff as one that refuses them
		_reconnect_backoff.OnFailure();
		return { REDIS_ERR, "Failed to execute command to redis" };
	}

	_reconnect_backoff.Reset();

	if (reply->type == REDIS_REPLY_ERROR)
	{
		logte("Failed to execute command to redis : %s:%d (err:%s)", _redis_ip.CStr(), _redis_port, reply->str);

		ov::String error_str = reply->str;
		freeReplyObject(reply);

		return { REDIS_ERR, error_str };
	}

	int reply_type = reply->type;
	ov::String reply_str;
	if (reply->type == REDIS_REPLY_STRING)
	{
		reply_str = reply->str;
	}
	else if (reply->type == REDIS_REPLY_INTEGER)
	{
		reply_str = ov::Converter::ToString((int64_t)reply->integer);
	}
	else if (reply->type == REDIS_REPLY_NIL)
	{
		reply_str = "NIL";
	}
	else if (reply->type == REDIS_REPLY_STATUS)
	{
		reply_str = reply->str;
	}
	else if (reply->type == REDIS_REPLY_ARRAY)
	{
		reply_str = ov::Converter::ToString((int)reply->elements) + " elements";
	}
	else
	{
		reply_str = "Unknown type";
	}

	freeReplyObject(reply);

	return { reply_type, reply_str };
}

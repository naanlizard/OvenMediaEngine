//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2022 AirenSoft. All rights reserved.
//
//==============================================================================

#include "origin_map_client.h"

#define OV_LOG_TAG "OriginMapClient"

namespace
{
	constexpr const char *kConnectFailedReply = "Failed to connect redis server";
	constexpr const char *kBackingOffReply = "Waiting to reconnect to redis server";

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
			RetryRegister();
			NofifyStreamsAlive();
			ProcessPendingUnregisters();
			return ov::DelayQueueAction::Repeat;
		},
		500);
	_update_timer.Start();
}

OriginMapClient::~OriginMapClient()
{
	// Declared before the state its callback uses, so the timer would otherwise stop after that state is gone
	_update_timer.Stop();

	if (_redis_context != nullptr)
	{
		redisFree(_redis_context);
		_redis_context = nullptr;
	}
}

bool OriginMapClient::NofifyStreamsAlive()
{
	std::unique_lock<std::recursive_mutex> lock(_origin_map_mutex);
	auto origin_map = _origin_map;
	lock.unlock();

	for (auto &[key, value] : origin_map)
	{
		Update(key, value);
	}

	return true;
}

bool OriginMapClient::RetryRegister()
{
	decltype(_origin_map_candidates) candidates;

	{
		std::unique_lock<std::recursive_mutex> lock(_origin_map_mutex);
		if (_origin_map_candidates.empty())
		{
			return true;
		}

		candidates.swap(_origin_map_candidates);
	}

	decltype(_origin_map_candidates) failed;

	for (auto &kv : candidates)
	{
		auto &key = kv.first;
		auto &value = kv.second;

		if (!Register(key, value))
		{
			failed.emplace(key, std::move(value));
		}
	}

	if (failed.empty() == false)
	{
		std::unique_lock<std::recursive_mutex> lock(_origin_map_mutex);
		for (auto &kv : failed)
		{
			_origin_map_candidates.try_emplace(kv.first, std::move(kv.second));
		}
	}

	return true;
}

bool OriginMapClient::ProcessPendingUnregisters()
{
	decltype(_origin_map_remove_candidates) candidates;

	{
		std::unique_lock<std::recursive_mutex> lock(_origin_map_mutex);
		if (_origin_map_remove_candidates.empty())
		{
			return true;
		}

		candidates.swap(_origin_map_remove_candidates);
	}

	for (auto &[app_stream_name, session_id] : candidates)
	{
		bool stale = true;
		bool registered = false;
		{
			std::lock_guard<std::recursive_mutex> lock(_origin_map_mutex);
			// Only unregister if the session ID matches: a different ID means the stream
			// reconnected and RequestRegister() has already started a new session.
			auto session_it = _session_map.find(app_stream_name);
			if (session_it != _session_map.end() && session_it->second == session_id)
			{
				stale = false;
				_origin_map_candidates.erase(app_stream_name);
				registered = _origin_map.erase(app_stream_name) > 0;
				_session_map.erase(session_it);
			}
		}
		if (registered)
		{
			Unregister(app_stream_name);
		}
		else if (stale)
		{
			logti("OriginMapStore: <%s> unregister skipped (reconnected)", app_stream_name.CStr());
		}
		else
		{
			logti("OriginMapStore: <%s> unregister skipped (not registered)", app_stream_name.CStr());
		}
	}

	return true;
}

bool OriginMapClient::AddOriginMap(const ov::String &app_stream_name, const ov::String &origin_host)
{
	std::lock_guard<std::recursive_mutex> lock(_origin_map_mutex);
	_origin_map[app_stream_name] = origin_host;
	return true;
}

bool OriginMapClient::AddOriginMapCandidate(const ov::String &app_stream_name, const ov::String &origin_host)
{
	std::lock_guard<std::recursive_mutex> lock(_origin_map_mutex);
	_origin_map_candidates[app_stream_name] = origin_host;
	return true;
}

bool OriginMapClient::RequestUnregister(const ov::String &app_stream_name)
{
	std::lock_guard<std::recursive_mutex> lock(_origin_map_mutex);
	_origin_map_remove_candidates.push_back({app_stream_name, _session_map[app_stream_name]});
	return true;
}

bool OriginMapClient::Register(const ov::String &app_stream_name, const ov::String &origin_host)
{
	bool same_registered = false;
	
	{
		// Check if already registered
		auto [reply_type, reply_str] = CommandToRedis(Replay::Allowed, "GET %s", app_stream_name.CStr());
		if (reply_type == REDIS_REPLY_NIL)
		{
			// Not exist, keep going
		}
		else if (reply_type == REDIS_REPLY_STRING)
		{
			if (origin_host == reply_str)
			{
				// Already registered with same origin host, so no need to register again.
				same_registered = true;
			}
			else
			{
				logte("<%s> stream is already registered with different origin host (%s)", app_stream_name.CStr(), reply_str.CStr());
				
				AddOriginMapCandidate(app_stream_name, origin_host);

				return false;
			}
		}
		else
		{
			logte_unless_unreachable(reply_type, reply_str, "Failed to get origin host from redis : %s:%d (err(%d):%s)", _redis_ip.CStr(), _redis_port, reply_type, reply_str.CStr());
			return false;
		}
	}

	if (same_registered == false)
	{
		// Set origin host to redis
		// The EXPIRE option is to prevent locking the app/stream when OvenMediaEngine unexpectedly stops.
		// So _update_timer updates the expire time once every 2.5 seconds.
		// If it is successful, it will return REDIS_REPLY_STATUS with "OK".
		auto [reply_type, reply_str] = CommandToRedis(Replay::Allowed, "SET %s %s EX %d NX", app_stream_name.CStr(), origin_host.CStr(), ORIGIN_MAP_STORE_KEY_EXPIRE_TIME);
		if (reply_type == REDIS_ERR || reply_type == REDIS_REPLY_ERROR)
		{
			logte_unless_unreachable(reply_type, reply_str, "Failed to register origin host to redis : %s:%d/%s/%s (err(%d):%s)", _redis_ip.CStr(), _redis_port, app_stream_name.CStr(), origin_host.CStr(), reply_type, reply_str.CStr());
			return false;
		}
		else if (reply_type == REDIS_REPLY_NIL)
		{
			logte("<%s> stream is already registered.", app_stream_name.CStr());
			AddOriginMapCandidate(app_stream_name, origin_host);
			return false;
		}
	}

	AddOriginMap(app_stream_name, origin_host);

	logti("OriginMapStore: <%s> stream is registered with origin host : %s", app_stream_name.CStr(), origin_host.CStr());

	return true;
}

bool OriginMapClient::RequestRegister(const ov::String &app_stream_name, const ov::String &origin_host)
{
	std::lock_guard<std::recursive_mutex> lock(_origin_map_mutex);
	// Increment session so any queued pending-unregister entry becomes stale and will be skipped
	// by ProcessPendingUnregisters() when it compares session IDs.
	_session_map[app_stream_name]++;
	_origin_map_candidates[app_stream_name] = origin_host;
	return true;
}

bool OriginMapClient::Update(const ov::String &app_stream_name, const ov::String &origin_host)
{
	// Set origin host to redis
	// XX option or EXPIRE cmd are not used because if redis server is restarted, update() can restore the origin stream info.
	auto [reply_type, reply_str] = CommandToRedis(Replay::Allowed, "SET %s %s EX %d", app_stream_name.CStr(), origin_host.CStr(), ORIGIN_MAP_STORE_KEY_EXPIRE_TIME);
	if (reply_type == REDIS_REPLY_STATUS && reply_str == "OK")
	{
		// Updated
	}
	else if (reply_type == REDIS_REPLY_NIL)
	{
		logte("Failed to update origin host to redis because the key does not exist : %s:%d (err(%d):%s)", _redis_ip.CStr(), _redis_port, reply_type, reply_str.CStr());
		return false;
	}
	else
	{
		logte_unless_unreachable(reply_type, reply_str, "Failed to update origin host to redis : %s:%d (err(%d):%s)", _redis_ip.CStr(), _redis_port, reply_type, reply_str.CStr());
		return false;
	}

	return true;
}

bool OriginMapClient::Unregister(const ov::String &app_stream_name)
{
	// A replayed DEL could remove the key of an origin that registered the stream after the first DEL landed.
	auto [reply_type, reply_str] = CommandToRedis(Replay::Forbidden, "DEL %s", app_stream_name.CStr());
	if (reply_type == REDIS_ERR || reply_type == REDIS_REPLY_ERROR)
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

// Caller holds _redis_context_mutex.
bool OriginMapClient::ConnectRedis()
{
	if (_redis_context != nullptr)
	{
		return true;
	}

	struct timeval connect_timeout = { ORIGIN_MAP_STORE_CONNECT_TIMEOUT_MS / 1000, (ORIGIN_MAP_STORE_CONNECT_TIMEOUT_MS % 1000) * 1000 };

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

	struct timeval command_timeout = { ORIGIN_MAP_STORE_COMMAND_TIMEOUT_MS / 1000, (ORIGIN_MAP_STORE_COMMAND_TIMEOUT_MS % 1000) * 1000 };
	redisSetTimeout(_redis_context, command_timeout);
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
		if (ConnectRedis() == false)
		{
			return { REDIS_ERR, kConnectFailedReply };
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

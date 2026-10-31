#include <gtest/gtest.h>
#include <modules/origin_map_client/origin_map_client.h>
#include <modules/origin_map_client/reconnect_backoff.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace std::chrono_literals;

	constexpr const char *kOk = "+OK\r\n";
	constexpr const char *kNil = "$-1\r\n";
	constexpr const char *kAuthRejected = "-ERR invalid password\r\n";
	constexpr auto kFirstAuthHold = 200ms;

	int CountClientErrorLines(const std::string &output)
	{
		std::istringstream lines(output);
		std::string line;
		int count = 0;

		while (std::getline(lines, line))
		{
			if ((line.find("] E [") != std::string::npos) && (line.find("OriginMapClient |") != std::string::npos))
			{
				count++;
			}
		}

		return count;
	}

	class FakeClock
	{
	public:
		std::chrono::steady_clock::time_point Now() const
		{
			return _now;
		}

		void Advance(std::chrono::milliseconds duration)
		{
			_now += duration;
		}

	private:
		std::chrono::steady_clock::time_point _now{std::chrono::hours(1)};
	};

	// A loopback server that speaks just enough RESP for OriginMapClient, one connection at a time.
	class FakeRedis
	{
	public:
		// Returns the raw reply, or nullopt to close the connection without replying.
		using Handler = std::function<std::optional<std::string>(int connection, int index, const std::string &name)>;
		using DetailedHandler = std::function<std::optional<std::string>(int connection, int index, const std::vector<std::string> &args)>;

		explicit FakeRedis(Handler handler)
			: FakeRedis(DetailedHandler([handler](int connection, int index, const std::vector<std::string> &args) {
				return handler(connection, index, args[0]);
			}))
		{
		}

		explicit FakeRedis(DetailedHandler handler)
			: _handler(std::move(handler))
		{
			_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);

			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			socklen_t length = sizeof(address);

			if ((::bind(_listen_fd, reinterpret_cast<sockaddr *>(&address), length) != 0) ||
				(::listen(_listen_fd, 8) != 0) ||
				(::getsockname(_listen_fd, reinterpret_cast<sockaddr *>(&address), &length) != 0))
			{
				ADD_FAILURE() << "cannot listen on loopback: " << strerror(errno);
			}

			_port = ntohs(address.sin_port);
			_thread = std::thread([this] { Run(); });
		}

		~FakeRedis()
		{
			_stop = true;
			_thread.join();
			::close(_listen_fd);
		}

		ov::String Host() const
		{
			return ov::String::FormatString("127.0.0.1:%d", _port);
		}

		int Connections() const
		{
			return _connections;
		}

		// "<connection>:<command>" in arrival order
		std::vector<std::string> Commands() const
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _commands;
		}

	private:
		bool WaitReadable(int fd) const
		{
			pollfd poll_fd{fd, POLLIN, 0};

			while (_stop == false)
			{
				if (::poll(&poll_fd, 1, 20) > 0)
				{
					return true;
				}
			}

			return false;
		}

		void Run()
		{
			while (WaitReadable(_listen_fd))
			{
				int fd = ::accept(_listen_fd, nullptr, nullptr);
				if (fd >= 0)
				{
					Serve(fd, _connections++);
					::close(fd);
				}
			}
		}

		void Serve(int fd, int connection)
		{
			std::string buffer;
			std::vector<std::string> args;
			int index = 0;

			while (WaitReadable(fd))
			{
				char chunk[1024];
				auto received = ::recv(fd, chunk, sizeof(chunk), 0);
				if (received <= 0)
				{
					return;
				}
				buffer.append(chunk, received);

				while (ParseCommand(buffer, args))
				{
					{
						std::lock_guard<std::mutex> lock(_mutex);
						_commands.push_back(std::to_string(connection) + ":" + args[0]);
					}

					auto reply = _handler(connection, index++, args);
					if (reply.has_value() == false)
					{
						return;
					}

					::send(fd, reply->data(), reply->size(), MSG_NOSIGNAL);
				}
			}
		}

		// Consumes one complete array of bulk strings from the front of the buffer.
		static bool ParseCommand(std::string &buffer, std::vector<std::string> &args)
		{
			size_t position = 0;
			std::string line;

			auto read_line = [&]() {
				auto end = buffer.find("\r\n", position);
				if (end == std::string::npos)
				{
					return false;
				}
				line = buffer.substr(position, end - position);
				position = end + 2;
				return true;
			};

			if ((read_line() == false) || (line.empty()) || (line[0] != '*'))
			{
				return false;
			}

			args.clear();
			auto count = std::stoi(line.substr(1));
			for (int i = 0; i < count; i++)
			{
				if ((read_line() == false) || (line.empty()) || (line[0] != '$'))
				{
					return false;
				}

				auto length = std::stoul(line.substr(1));
				if (buffer.size() < position + length + 2)
				{
					return false;
				}

				args.push_back(buffer.substr(position, length));
				position += length + 2;
			}

			buffer.erase(0, position);
			return (args.empty() == false);
		}

		DetailedHandler _handler;
		int _listen_fd = -1;
		uint16_t _port = 0;
		std::atomic<bool> _stop{false};
		std::atomic<int> _connections{0};
		mutable std::mutex _mutex;
		std::vector<std::string> _commands;
		std::thread _thread;
	};
}  // namespace

TEST(OriginMapReconnectBackoff, NotBackingOffBeforeAnyFailure)
{
	FakeClock clock;
	OriginMapReconnectBackoff backoff([&clock] { return clock.Now(); });

	EXPECT_FALSE(backoff.IsBackingOff());
}

TEST(OriginMapReconnectBackoff, FailureOpensOneSecondWindow)
{
	FakeClock clock;
	OriginMapReconnectBackoff backoff([&clock] { return clock.Now(); });

	backoff.OnFailure();
	EXPECT_TRUE(backoff.IsBackingOff());

	clock.Advance(999ms);
	EXPECT_TRUE(backoff.IsBackingOff());

	clock.Advance(1ms);
	EXPECT_FALSE(backoff.IsBackingOff());
}

TEST(OriginMapReconnectBackoff, ConsecutiveFailuresDoubleTheWindowUpToTheCap)
{
	FakeClock clock;
	OriginMapReconnectBackoff backoff([&clock] { return clock.Now(); });

	std::vector<int64_t> windows;
	for (int i = 0; i < 6; i++)
	{
		backoff.OnFailure();
		windows.push_back(backoff.GetDelay().count());

		clock.Advance(backoff.GetDelay() - 1ms);
		EXPECT_TRUE(backoff.IsBackingOff());

		clock.Advance(1ms);
		EXPECT_FALSE(backoff.IsBackingOff());
	}

	EXPECT_EQ(windows, (std::vector<int64_t>{1000, 2000, 4000, 5000, 5000, 5000}));
}

TEST(OriginMapReconnectBackoff, ResetEndsTheWindow)
{
	FakeClock clock;
	OriginMapReconnectBackoff backoff([&clock] { return clock.Now(); });

	backoff.OnFailure();
	clock.Advance(1000ms);
	backoff.OnFailure();
	clock.Advance(2000ms);

	backoff.Reset();
	EXPECT_FALSE(backoff.IsBackingOff());

	backoff.OnFailure();
	EXPECT_EQ(backoff.GetDelay(), 1000ms);
}

TEST(OriginMapClient, RejectedAuthSkipsReconnectInsideBackoff)
{
	FakeRedis redis([](int, int, const std::string &) { return kAuthRejected; });
	OriginMapClient client(redis.Host(), "test-password");
	ov::String origin;

	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);

	EXPECT_EQ(redis.Connections(), 1);
	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:AUTH"}));
}

TEST(OriginMapClient, CallerQueuedBehindFailingConnectReturnsWithoutConnecting)
{
	std::promise<void> first_auth;
	auto first_auth_received = first_auth.get_future();

	FakeRedis redis([&first_auth](int connection, int index, const std::string &) {
		if ((connection == 0) && (index == 0))
		{
			first_auth.set_value();
			std::this_thread::sleep_for(kFirstAuthHold);
		}
		return kAuthRejected;
	});
	OriginMapClient client(redis.Host(), "test-password");

	auto first = std::async(std::launch::async, [&client] {
		ov::String origin;
		return client.GetOrigin("app/first", origin);
	});
	ASSERT_EQ(first_auth_received.wait_for(5s), std::future_status::ready);

	ov::String origin;
	EXPECT_EQ(client.GetOrigin("app/second", origin), CommonErrorCode::ERROR);
	EXPECT_EQ(first.get(), CommonErrorCode::ERROR);

	EXPECT_EQ(redis.Connections(), 1);
	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:AUTH"}));
}

TEST(OriginMapClient, CommandOnLiveConnectionSendsNoPing)
{
	FakeRedis redis([](int, int, const std::string &name) {
		return (name == "PING") ? "+PONG\r\n" : kNil;
	});
	OriginMapClient client(redis.Host(), "");
	ov::String origin;

	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::NOT_FOUND);
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::NOT_FOUND);

	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:GET", "0:GET"}));
}

TEST(OriginMapClient, DroppedConnectionRetriesTheCommandOnANewConnection)
{
	FakeRedis redis([](int connection, int index, const std::string &) -> std::optional<std::string> {
		if ((connection == 0) && (index > 0))
		{
			return std::nullopt;
		}
		return kNil;
	});
	OriginMapClient client(redis.Host(), "");
	ov::String origin;

	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::NOT_FOUND);
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::NOT_FOUND);

	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:GET", "0:GET", "1:GET"}));
}

TEST(OriginMapClient, DroppedConnectionRetriesOnceThenBacksOff)
{
	FakeRedis redis([](int connection, int index, const std::string &) -> std::optional<std::string> {
		if ((connection == 0) && (index == 0))
		{
			return kNil;
		}
		return std::nullopt;
	});
	OriginMapClient client(redis.Host(), "");
	ov::String origin;

	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::NOT_FOUND);
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);
	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:GET", "0:GET", "1:GET"}));

	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);
	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:GET", "0:GET", "1:GET"}));
	EXPECT_EQ(redis.Connections(), 2);
}

TEST(OriginMapClient, FailedReconnectAfterDroppedConnectionOpensBackoff)
{
	FakeRedis redis([](int connection, int index, const std::string &) -> std::optional<std::string> {
		if (connection == 0)
		{
			if (index == 0)
			{
				return kOk;
			}
			if (index == 1)
			{
				return kNil;
			}
			return std::nullopt;
		}
		return kAuthRejected;
	});
	OriginMapClient client(redis.Host(), "test-password");
	ov::String origin;

	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::NOT_FOUND);
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);

	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:AUTH", "0:GET", "0:GET", "1:AUTH"}));
}

TEST(OriginMapClient, UnregisterDoesNotReplayADeleteWhoseReplyWasLost)
{
	std::promise<void> registered;
	auto registered_received = registered.get_future();
	std::promise<void> first_delete;
	auto delete_received = first_delete.get_future();

	std::atomic<int> deletes{0};
	FakeRedis redis(FakeRedis::DetailedHandler([&, refreshes = 0](int, int, const std::vector<std::string> &args) mutable -> std::optional<std::string> {
		if (args[0] == "EVAL")
		{
			if (args.size() == 6)
			{
				if (refreshes++ == 0)
				{
					registered.set_value();
				}
				return ":1\r\n";
			}
			if (deletes++ == 0)
			{
				first_delete.set_value();
			}
			return std::nullopt;
		}
		return kNil;
	}));
	OriginMapClient client(redis.Host(), "");
	client.RequestRegister("app/stream", "ovt://origin-a:9000/app/stream");
	ASSERT_EQ(registered_received.wait_for(5s), std::future_status::ready);
	client.RequestUnregister("app/stream");
	ASSERT_EQ(delete_received.wait_for(5s), std::future_status::ready);

	// The lost reply opens the backoff window, so this sends nothing
	ov::String origin;
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);
	EXPECT_EQ(deletes.load(), 1);
	EXPECT_EQ(redis.Commands().back(), "0:EVAL");
	EXPECT_EQ(redis.Connections(), 1);
}

TEST(OriginMapClient, UnregisterDuringRegistrationDoesNotLeaveAZombieOwner)
{
	std::promise<void> registering;
	std::promise<void> proceed;
	std::promise<void> removed;
	auto registering_received = registering.get_future();
	auto proceed_received = proceed.get_future();
	auto removed_received = removed.get_future();
	std::atomic<int> refreshes{0};
	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] == "EVAL" && args.size() == 6)
		{
			if (refreshes++ == 0)
			{
				registering.set_value();
				proceed_received.wait_for(5s);
			}
			return ":1\r\n";
		}
		if (args[0] == "EVAL")
		{
			EXPECT_EQ(args[4], "ovt://origin-a:9000/app/stream");
			removed.set_value();
			return ":1\r\n";
		}
		return kNil;
	}));
	OriginMapClient client(redis.Host(), "");
	client.RequestRegister("app/stream", "ovt://origin-a:9000/app/stream");
	ASSERT_EQ(registering_received.wait_for(5s), std::future_status::ready);
	client.RequestUnregister("app/stream");
	proceed.set_value();
	ASSERT_EQ(removed_received.wait_for(5s), std::future_status::ready);
	auto after_removal = refreshes.load();
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(refreshes.load(), after_removal);
}

TEST(OriginMapClient, LogsOneErrorLinePerBackoffWindow)
{
	FakeRedis redis([](int, int, const std::string &) { return kAuthRejected; });
	OriginMapClient client(redis.Host(), "test-password");
	ov::String origin;

	testing::internal::CaptureStderr();
	for (int i = 0; i < 5; i++)
	{
		EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);
	}
	auto output = testing::internal::GetCapturedStderr();

	EXPECT_EQ(CountClientErrorLines(output), 1) << output;
	EXPECT_NE(output.find("next attempt in 1000 ms"), std::string::npos) << output;
	EXPECT_EQ(redis.Connections(), 1);
}

TEST(OriginMapClient, DestructorReleasesARegistrationWhoseReplyWasLost)
{
	std::promise<void> applied;
	auto applied_received = applied.get_future();
	std::atomic<int> removals{0};
	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] == "EVAL" && args.size() == 6)
		{
			applied.set_value();
			return std::nullopt;
		}
		if (args[0] == "EVAL")
		{
			EXPECT_EQ(args[3], "app/stream");
			EXPECT_EQ(args[4], "ovt://origin-a:9000/app/stream");
			++removals;
			return ":1\r\n";
		}
		return kNil;
	}));
	{
		OriginMapClient client(redis.Host(), "");
		client.RequestRegister("app/stream", "ovt://origin-a:9000/app/stream");
		ASSERT_EQ(applied_received.wait_for(5s), std::future_status::ready);
	}
	EXPECT_EQ(removals.load(), 1);
	EXPECT_EQ(redis.Commands(), (std::vector<std::string>{"0:EVAL", "1:EVAL"}));
}

TEST(OriginMapClient, ReregisterBeforeQueuedUnregisterRunsKeepsTheEntry)
{
	std::promise<void> refreshing;
	std::promise<void> proceed;
	auto refreshing_received = refreshing.get_future();
	auto proceed_received = proceed.get_future();
	std::atomic<int> refreshes{0};
	std::atomic<int> removals{0};
	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] == "EVAL" && args.size() == 6)
		{
			// The first keepalive holds the timer while both requests are queued
			if (refreshes++ == 1)
			{
				refreshing.set_value();
				proceed_received.wait_for(5s);
			}
			return ":1\r\n";
		}
		if (args[0] == "EVAL")
		{
			++removals;
			return ":1\r\n";
		}
		return kNil;
	}));
	OriginMapClient client(redis.Host(), "");
	client.RequestRegister("app/stream", "ovt://origin-a:9000/app/stream");
	ASSERT_EQ(refreshing_received.wait_for(5s), std::future_status::ready);
	client.RequestUnregister("app/stream");
	client.RequestRegister("app/stream", "ovt://origin-a:9000/app/stream");
	proceed.set_value();
	auto resumed = refreshes.load();
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(removals.load(), 0);
	EXPECT_GT(refreshes.load(), resumed);
}

TEST(OriginMapClient, RequestsQueuedDuringAPassWaitForTheRefresh)
{
	std::mutex events_mutex;
	std::vector<std::string> events;
	std::promise<void> refreshed, first_delete, proceed, second_delete;
	auto refreshed_received = refreshed.get_future();
	auto first_delete_received = first_delete.get_future();
	auto proceed_received = proceed.get_future().share();
	auto second_delete_received = second_delete.get_future();
	std::atomic<int> refreshes{0}, deletes{0};

	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] != "EVAL")
		{
			return kNil;
		}

		{
			std::lock_guard<std::mutex> lock(events_mutex);
			events.push_back(((args.size() == 6) ? "refresh:" : "delete:") + args[3]);
		}

		if (args.size() == 6)
		{
			if ((args[3] == "app/a") && (refreshes++ == 0))
			{
				refreshed.set_value();
			}
			return ":1\r\n";
		}

		auto index = deletes++;
		if (index == 0)
		{
			first_delete.set_value();
			proceed_received.wait_for(5s);
		}
		else if (index == 1)
		{
			second_delete.set_value();
		}
		return ":1\r\n";
	}));
	OriginMapClient client(redis.Host(), "");

	client.RequestRegister("app/a", "ovt://origin-a:9000/app/a");
	ASSERT_EQ(refreshed_received.wait_for(5s), std::future_status::ready);

	client.RequestRegister("app/b", "ovt://origin-a:9000/app/b");
	client.RequestUnregister("app/b");
	ASSERT_EQ(first_delete_received.wait_for(5s), std::future_status::ready);

	// Queued while the pass is busy with the first delete
	client.RequestRegister("app/c", "ovt://origin-a:9000/app/c");
	client.RequestUnregister("app/c");
	proceed.set_value();
	ASSERT_EQ(second_delete_received.wait_for(5s), std::future_status::ready);

	std::lock_guard<std::mutex> lock(events_mutex);
	auto first = std::find(events.begin(), events.end(), "delete:app/b");
	auto second = std::find(events.begin(), events.end(), "delete:app/c");
	ASSERT_NE(first, events.end());
	ASSERT_NE(second, events.end());
	EXPECT_NE(std::find(first, second, "refresh:app/a"), second);
}

TEST(OriginMapClient, NewUrlForAKeyRetiresTheOldOne)
{
	const std::string old_origin = "ovt://origin-a:9000/app/stream";
	std::promise<void> owned, retired;
	auto owned_received = owned.get_future();
	auto retired_received = retired.get_future();
	std::atomic<int> old_refreshes{0};
	std::atomic<bool> old_deleted{false};

	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] != "EVAL")
		{
			return kNil;
		}

		if ((args.size() == 6) && (args[4] == old_origin) && (old_refreshes++ == 0))
		{
			owned.set_value();
		}
		else if ((args.size() == 5) && (args[4] == old_origin) && (old_deleted.exchange(true) == false))
		{
			retired.set_value();
		}
		return ":1\r\n";
	}));
	OriginMapClient client(redis.Host(), "");

	client.RequestRegister("app/stream", old_origin.c_str());
	ASSERT_EQ(owned_received.wait_for(5s), std::future_status::ready);

	client.RequestRegister("app/stream", "ovt://origin-b:9000/app/stream");
	client.RequestUnregister("app/stream");
	ASSERT_EQ(retired_received.wait_for(5s), std::future_status::ready);

	auto after_retire = old_refreshes.load();
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(old_refreshes.load(), after_retire);
}

TEST(OriginMapClient, NewUrlRetiresARegistrationWhoseReplyWasLost)
{
	const std::string old_origin = "ovt://origin-a:9000/app/stream";
	std::promise<void> retried, retired;
	auto retried_received = retried.get_future();
	auto retired_received = retired.get_future();
	std::atomic<int> old_attempts{0};
	std::atomic<bool> old_deleted{false};

	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] != "EVAL")
		{
			return kNil;
		}

		if ((args.size() == 6) && (args[4] == old_origin))
		{
			auto attempt = old_attempts++;
			if (attempt == 0)
			{
				// Applied, but the reply is lost
				return std::nullopt;
			}
			if (attempt == 1)
			{
				retried.set_value();
			}
			return ":0\r\n";
		}

		if ((args.size() == 5) && (args[4] == old_origin) && (old_deleted.exchange(true) == false))
		{
			retired.set_value();
		}
		return ":1\r\n";
	}));
	OriginMapClient client(redis.Host(), "");

	client.RequestRegister("app/stream", old_origin.c_str());
	ASSERT_EQ(retried_received.wait_for(5s), std::future_status::ready);

	client.RequestRegister("app/stream", "ovt://origin-b:9000/app/stream");
	EXPECT_EQ(retired_received.wait_for(5s), std::future_status::ready);
}

TEST(OriginMapClient, DestructorCleanupEndsWithinItsBudget)
{
	std::promise<void> registered;
	auto registered_received = registered.get_future();
	std::atomic<int> refreshes{0};

	FakeRedis redis(FakeRedis::DetailedHandler([&](int, int, const std::vector<std::string> &args) -> std::optional<std::string> {
		if (args[0] != "EVAL")
		{
			return kNil;
		}

		if (args.size() == 6)
		{
			if (++refreshes == 3)
			{
				registered.set_value();
			}
			return ":1\r\n";
		}

		// Each delete answers just inside the command timeout
		std::this_thread::sleep_for(2500ms);
		return ":1\r\n";
	}));
	auto client = std::make_unique<OriginMapClient>(redis.Host(), "");

	for (const char *name : {"app/a", "app/b", "app/c"})
	{
		client->RequestRegister(name, ov::String::FormatString("ovt://origin-a:9000/%s", name));
	}
	ASSERT_EQ(registered_received.wait_for(5s), std::future_status::ready);

	auto started = std::chrono::steady_clock::now();
	client.reset();
	EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
}

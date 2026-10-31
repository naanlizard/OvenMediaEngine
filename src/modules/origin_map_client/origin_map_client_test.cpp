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

		explicit FakeRedis(Handler handler)
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

					auto reply = _handler(connection, index++, args[0]);
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

		Handler _handler;
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

TEST(OriginMapClient, UnregisterDoesNotResendADelWhoseReplyWasLost)
{
	std::promise<void> registered;
	std::promise<void> first_del;
	auto registered_received = registered.get_future();
	auto first_del_received = first_del.get_future();

	FakeRedis redis([&registered, &first_del, sets = 0, dels = 0](int, int, const std::string &name) mutable -> std::optional<std::string> {
		if (name == "SET")
		{
			if (sets++ == 0)
			{
				registered.set_value();
			}
			return kOk;
		}

		if (name == "DEL")
		{
			if (dels++ == 0)
			{
				first_del.set_value();
				return std::nullopt;
			}
			return ":1\r\n";
		}

		return kNil;
	});
	OriginMapClient client(redis.Host(), "");

	client.RequestRegister("app/stream", "ovt://origin-a:9000/app/stream");
	ASSERT_EQ(registered_received.wait_for(5s), std::future_status::ready);
	client.RequestUnregister("app/stream");
	ASSERT_EQ(first_del_received.wait_for(5s), std::future_status::ready);

	// The lost reply opens the backoff window, so this sends nothing
	ov::String origin;
	EXPECT_EQ(client.GetOrigin("app/stream", origin), CommonErrorCode::ERROR);

	auto commands = redis.Commands();
	auto del = std::find(commands.begin(), commands.end(), "0:DEL");
	ASSERT_NE(del, commands.end());
	EXPECT_EQ(std::vector<std::string>(del, commands.end()), (std::vector<std::string>{"0:DEL"}));
	EXPECT_EQ(redis.Connections(), 1);
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

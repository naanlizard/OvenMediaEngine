#include <gtest/gtest.h>
#include <modules/origin_map_client/origin_map_client.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unistd.h>

namespace
{
	using namespace std::chrono_literals;

	// Runs against a disposable Redis named by OME_TEST_REDIS, such as 127.0.0.1:6379
	class OriginMapOwnership : public testing::Test
	{
	protected:
		using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

		void SetUp() override
		{
			const char *host = std::getenv("OME_TEST_REDIS");
			if (host == nullptr)
			{
				GTEST_SKIP() << "Set OME_TEST_REDIS to a disposable Redis for ownership integration tests";
			}
			_host = host;
			auto address = _host.Split(":");
			ASSERT_EQ(address.size(), 2U);
			struct timeval timeout = {1, 0};
			_context = redisConnectWithTimeout(address[0].CStr(), ov::Converter::ToUInt16(address[1]), timeout);
			ASSERT_NE(_context, nullptr);
			ASSERT_EQ(_context->err, 0) << _context->errstr;
			redisSetTimeout(_context, timeout);
			_key = ov::String::FormatString("ome-owner-test/%d/%s", getpid(), testing::UnitTest::GetInstance()->current_test_info()->name());
			Command("DEL %s", _key.CStr());
		}

		void TearDown() override
		{
			if (_context != nullptr)
			{
				if (!_key.IsEmpty())
				{
					Command("DEL %s", _key.CStr());
				}
				redisFree(_context);
			}
		}

		template <typename... Args>
		Reply Command(const char *format, Args... args)
		{
			auto reply = Reply(static_cast<redisReply *>(redisCommand(_context, format, args...)), freeReplyObject);
			EXPECT_NE(reply, nullptr);
			return reply;
		}

		std::string Owner()
		{
			auto reply = Command("GET %s", _key.CStr());
			if (!reply)
			{
				return "<transport error>";
			}
			EXPECT_TRUE(reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_NIL)
				<< "Unexpected Redis reply type " << reply->type;
			return reply->type == REDIS_REPLY_STRING ? reply->str :
				(reply->type == REDIS_REPLY_NIL ? "" : "<unexpected reply>");
		}

		bool WaitForOwner(const char *expected)
		{
			auto deadline = std::chrono::steady_clock::now() + 4s;
			do
			{
				if (Owner() == expected)
				{
					return true;
				}
				std::this_thread::sleep_for(20ms);
			} while (std::chrono::steady_clock::now() < deadline);
			return false;
		}

		static constexpr const char *kOld = "ovt://origin-a:9000/app/stream";
		static constexpr const char *kNew = "ovt://origin-b:9000/app/stream";
		ov::String _host;
		ov::String _key;
		redisContext *_context = nullptr;
	};
}

TEST_F(OriginMapOwnership, RefreshCannotOverwriteOrExtendReplacementOwner)
{
	OriginMapClient client(_host, "");
	client.RequestRegister(_key, kOld);
	ASSERT_TRUE(WaitForOwner(kOld));
	Command("SET %s %s PX 8000", _key.CStr(), kNew);
	std::this_thread::sleep_for(1200ms);
	EXPECT_EQ(Owner(), kNew);
	auto ttl = Command("PTTL %s", _key.CStr());
	ASSERT_NE(ttl, nullptr);
	ASSERT_EQ(ttl->type, REDIS_REPLY_INTEGER);
	EXPECT_GT(ttl->integer, 0);
	EXPECT_LT(ttl->integer, 7500);
}

TEST_F(OriginMapOwnership, RefreshRestoresAnAbsentKeyAfterRedisRecovery)
{
	OriginMapClient client(_host, "");
	client.RequestRegister(_key, kOld);
	ASSERT_TRUE(WaitForOwner(kOld));
	Command("DEL %s", _key.CStr());
	ASSERT_TRUE(WaitForOwner(kOld));
	auto ttl = Command("PTTL %s", _key.CStr());
	ASSERT_NE(ttl, nullptr);
	ASSERT_EQ(ttl->type, REDIS_REPLY_INTEGER);
	EXPECT_GT(ttl->integer, 8000);
}

TEST_F(OriginMapOwnership, UnregisterCannotDeleteReplacementOwner)
{
	OriginMapClient client(_host, "");
	client.RequestRegister(_key, kOld);
	ASSERT_TRUE(WaitForOwner(kOld));
	Command("SET %s %s EX 10", _key.CStr(), kNew);
	client.RequestUnregister(_key);
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(Owner(), kNew);
}

TEST_F(OriginMapOwnership, QueuedUnregisterBeforeReregisterPreservesTheNewRequest)
{
	OriginMapClient client(_host, "");
	client.RequestRegister(_key, kOld);
	client.RequestUnregister(_key);
	client.RequestRegister(_key, kNew);
	ASSERT_TRUE(WaitForOwner(kNew));
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(Owner(), kNew);
}

TEST_F(OriginMapOwnership, QueuedUnregisterBeforeSameUrlReregisterPreservesTheNewRequest)
{
	OriginMapClient client(_host, "");
	client.RequestRegister(_key, kOld);
	client.RequestUnregister(_key);
	client.RequestRegister(_key, kOld);
	ASSERT_TRUE(WaitForOwner(kOld));
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(Owner(), kOld);
}

TEST_F(OriginMapOwnership, DestructorReleasesOwnedAndPendingRemovalKeys)
{
	{
		OriginMapClient client(_host, "");
		client.RequestRegister(_key, kOld);
		ASSERT_TRUE(WaitForOwner(kOld));
		client.RequestUnregister(_key);
	}
	EXPECT_EQ(Owner(), "");
	{
		OriginMapClient client(_host, "");
		client.RequestRegister(_key, kOld);
		ASSERT_TRUE(WaitForOwner(kOld));
	}
	EXPECT_EQ(Owner(), "");
}

TEST_F(OriginMapOwnership, DestructorCannotDeleteReplacementOwner)
{
	{
		OriginMapClient client(_host, "");
		client.RequestRegister(_key, kOld);
		ASSERT_TRUE(WaitForOwner(kOld));
		Command("SET %s %s EX 10", _key.CStr(), kNew);
	}
	EXPECT_EQ(Owner(), kNew);
}

TEST_F(OriginMapOwnership, DestructorDoesNotRegisterUnprocessedRequests)
{
	{
		OriginMapClient client(_host, "");
		client.RequestRegister(_key, kOld);
	}
	EXPECT_EQ(Owner(), "");
}

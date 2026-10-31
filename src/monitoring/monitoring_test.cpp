#include "monitoring.h"

#include <gtest/gtest.h>
#include <thread>

namespace
{
	class TestHost : public info::Host
	{
	public:
		explicit TestHost(info::host_id_t id) : Host("test", "test", cfg::vhost::VirtualHost())
		{
			_host_id = id;
		}
	};

	class TestApplication : public info::Application
	{
	public:
		TestApplication(const info::Host &host, info::application_id_t id, const char *name)
			: Application(host, id, info::VHostAppName("test", name), false) {}
	};

	class MonitoringBytesOut : public testing::Test
	{
	protected:
		void SetUp() override
		{
			monitor->OnServerStarted(std::make_shared<cfg::Server>());
		}

		void TearDown() override
		{
			monitor->Release();
		}

		void Register(const info::Stream &stream)
		{
			ASSERT_TRUE(monitor->OnHostCreated(stream.GetApplicationInfo().GetHostInfo()));
			ASSERT_TRUE(monitor->OnApplicationCreated(stream.GetApplicationInfo()));
			ASSERT_TRUE(monitor->OnStreamCreated(stream));
		}

		void ExpectSeparateCounters(const info::Stream &first, const info::Stream &second)
		{
			Register(first);
			Register(second);
			auto first_metric = monitor->GetStreamMetrics(first);
			auto second_metric = monitor->GetStreamMetrics(second);
			ASSERT_NE(first_metric, nullptr);
			ASSERT_NE(second_metric, nullptr);
			ASSERT_NE(first_metric, second_metric);

			monitor->IncreaseBytesOut(first, PublisherType::LLHls, 100);
			monitor->IncreaseBytesOut(first, PublisherType::LLHls, 10);
			monitor->IncreaseBytesOut(second, PublisherType::LLHls, 200);
			monitor->IncreaseBytesOut(second, PublisherType::LLHls, 20);
			monitor->IncreaseBytesOut(first, PublisherType::LLHls, 1);

			EXPECT_EQ(first_metric->GetTotalBytesOut(), 111u);
			EXPECT_EQ(second_metric->GetTotalBytesOut(), 220u);
			EXPECT_EQ(monitor->GetApplicationMetrics(first.GetApplicationInfo())->GetTotalBytesOut(), 111u);
			EXPECT_EQ(monitor->GetApplicationMetrics(second.GetApplicationInfo())->GetTotalBytesOut(), 220u);
			EXPECT_EQ(monitor->GetServerMetrics()->GetTotalBytesOut(), 331u);
		}

		mon::Monitoring *monitor = mon::Monitoring::GetInstance();
	};

	TEST_F(MonitoringBytesOut, SameStreamIdInDifferentApplications)
	{
		TestHost host(1);
		TestApplication first_app(host, 1, "first");
		TestApplication second_app(host, 2, "second");
		info::Stream first(first_app, 42, StreamSourceType::Ovt);
		info::Stream second(second_app, 42, StreamSourceType::Ovt);
		ExpectSeparateCounters(first, second);
		EXPECT_EQ(monitor->GetHostMetrics(host)->GetTotalBytesOut(), 331u);
	}

	TEST_F(MonitoringBytesOut, SameApplicationAndStreamIdsInDifferentHosts)
	{
		TestHost first_host(1), second_host(2);
		TestApplication first_app(first_host, 1, "app");
		TestApplication second_app(second_host, 1, "app");
		info::Stream first(first_app, 42, StreamSourceType::Ovt);
		info::Stream second(second_app, 42, StreamSourceType::Ovt);
		ExpectSeparateCounters(first, second);
		EXPECT_EQ(monitor->GetHostMetrics(first_host)->GetTotalBytesOut(), 111u);
		EXPECT_EQ(monitor->GetHostMetrics(second_host)->GetTotalBytesOut(), 220u);
	}

	TEST_F(MonitoringBytesOut, OutputStreamCountsIntoItsInputStreamUntilInputIsDeleted)
	{
		TestHost host(1);
		TestApplication app(host, 1, "app");
		auto input = std::make_shared<info::Stream>(app, 42, StreamSourceType::Ovt);
		info::Stream output(app, 43, StreamSourceType::Transcoder);
		output.LinkInputStream(input);
		Register(*input);
		Register(output);
		auto input_metric = monitor->GetStreamMetrics(*input);
		ASSERT_NE(input_metric, nullptr);

		const auto before_send = std::chrono::steady_clock::now();
		monitor->IncreaseBytesOut(output, PublisherType::LLHls, 100);
		monitor->IncreaseBytesOut(output, PublisherType::LLHls, 10);
		EXPECT_EQ(input_metric->GetTotalBytesOut(), 110u);
		EXPECT_GE(input_metric->GetLastSentTimeSteady(), before_send);

		ASSERT_TRUE(monitor->OnStreamDeleted(*input));
		monitor->IncreaseBytesOut(output, PublisherType::LLHls, 200);
		EXPECT_EQ(input_metric->GetTotalBytesOut(), 110u);
		EXPECT_EQ(monitor->GetStreamMetrics(output)->GetTotalBytesOut(), 310u);
	}

	enum class RemovedMetrics { Stream, Application, Host, Server };
	class MonitoringCacheRemoval : public MonitoringBytesOut, public testing::WithParamInterface<RemovedMetrics> {};

	TEST_P(MonitoringCacheRemoval, IgnoresRemovedMetricAndUsesReplacement)
	{
		TestHost host(1);
		TestApplication app(host, 1, "app");
		info::Stream stream(app, 42, StreamSourceType::Ovt);
		Register(stream);
		// Keep the removed object alive: weak_ptr::lock() alone cannot detect removal.
		auto removed_metric = monitor->GetStreamMetrics(stream);
		ASSERT_NE(removed_metric, nullptr);
		monitor->IncreaseBytesOut(stream, PublisherType::LLHls, 100);

		// Removal on a different thread must invalidate this thread's cache too.
		std::thread remover([&] {
			switch (GetParam())
			{
				case RemovedMetrics::Stream: EXPECT_TRUE(monitor->OnStreamDeleted(stream)); break;
				case RemovedMetrics::Application: EXPECT_TRUE(monitor->OnApplicationDeleted(app)); break;
				case RemovedMetrics::Host: EXPECT_TRUE(monitor->OnHostDeleted(host)); break;
				case RemovedMetrics::Server: monitor->Release(); break;
			}
		});
		remover.join();
		monitor->IncreaseBytesOut(stream, PublisherType::LLHls, 200);
		EXPECT_EQ(removed_metric->GetTotalBytesOut(), 100u);

		if (GetParam() == RemovedMetrics::Server)
		{
			monitor->OnServerStarted(std::make_shared<cfg::Server>());
		}
		Register(stream);
		auto replacement = monitor->GetStreamMetrics(stream);
		ASSERT_NE(replacement, nullptr);
		ASSERT_NE(replacement, removed_metric);
		monitor->IncreaseBytesOut(stream, PublisherType::LLHls, 300);
		EXPECT_EQ(removed_metric->GetTotalBytesOut(), 100u);
		EXPECT_EQ(replacement->GetTotalBytesOut(), 300u);
		// An erased application can still own its old streams; release that cycle.
		if (GetParam() == RemovedMetrics::Application)
		{
			removed_metric->GetApplicationMetrics()->Release();
		}
	}

	INSTANTIATE_TEST_SUITE_P(Lifecycle, MonitoringCacheRemoval,
		testing::Values(RemovedMetrics::Stream, RemovedMetrics::Application, RemovedMetrics::Host, RemovedMetrics::Server));
} // namespace

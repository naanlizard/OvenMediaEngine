#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "ovt_provider.h"

// The first pull of each stream asks for the shared client socket pool; several can start at the same moment.
TEST(OvtProviderClientSocketPool, ConcurrentFirstCallsShareOneInitializedPool)
{
	constexpr int kRounds = 50;
	constexpr int kCallers = 8;
	cfg::Server server_config;

	for (int round = 0; round < kRounds; round++)
	{
		auto provider = std::make_shared<pvd::OvtProvider>(server_config, nullptr);

		std::atomic<int> arrived{0};
		std::vector<std::shared_ptr<ov::SocketPool>> pools(kCallers);
		std::vector<int> worker_counts(kCallers, -1);
		std::vector<std::thread> callers;
		for (int i = 0; i < kCallers; i++)
		{
			callers.emplace_back([&, i]() {
				arrived.fetch_add(1);
				while (arrived.load() < kCallers)
				{
					std::this_thread::yield();
				}
				pools[i] = provider->GetClientSocketPool();
				worker_counts[i] = (pools[i] != nullptr) ? pools[i]->GetWorkerCount() : -1;
			});
		}
		for (auto &caller : callers)
		{
			caller.join();
		}

		// The default configuration has one OVT client worker
		ASSERT_NE(pools[0].get(), nullptr) << "round " << round;
		for (int i = 0; i < kCallers; i++)
		{
			ASSERT_EQ(pools[i].get(), pools[0].get()) << "round " << round << ", caller " << i;
			ASSERT_EQ(worker_counts[i], 1) << "round " << round << ", caller " << i;
		}
	}
}

#include <gtest/gtest.h>
#include <config/items/items.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>

#define protected public
#include <config/config_manager.h>
#undef protected
#include "virtual_host.h"

namespace
{
	template<typename T>
	bool Parse(const char *name, const std::string &xml, T &item)
	{
		char path[] = "/tmp/ome_origin_port_test_XXXXXX";
		int fd = mkstemp(path);
		if (fd < 0) return false;
		close(fd);
		{
			std::ofstream output(path);
			output << xml;
		}
		bool parsed = true;
		try
		{
			cfg::DataSource source(cfg::DataType::Xml, path, cfg::ItemName(name));
			item.FromDataSource(name, cfg::ItemName(name), source);
		}
		catch (const cfg::ConfigError &)
		{
			parsed = false;
		}
		remove(path);
		return parsed;
	}

	std::string StoreXml(const std::string &port, bool host = true)
	{
		return "<OriginMapStore><RedisServer><Host>127.0.0.1:6379</Host></RedisServer>" +
			std::string(host ? "<OriginHostName>192.0.2.10</OriginHostName>" : "") + port + "</OriginMapStore>";
	}

	class OriginAdvertisement : public testing::Test
	{
	protected:
		void SetUp() override
		{
			previous_config = cfg::ConfigManager::GetInstance()->_server;
			auto server = std::make_shared<cfg::Server>();
			ASSERT_TRUE(Parse("Server", "<Server version=\"8\"><Name>test</Name><Type>origin</Type><IP>127.0.0.1</IP>"
				"<Bind><Publishers><OVT><Port>9999</Port></OVT></Publishers></Bind></Server>", *server));
			cfg::ConfigManager::GetInstance()->_server = server;
		}

		void TearDown() override
		{
			cfg::ConfigManager::GetInstance()->_server = previous_config;
		}

		std::unique_ptr<ocst::VirtualHost> MakeHost(const std::string &port, bool host = true)
		{
			cfg::vhost::VirtualHost config;
			if (!Parse("VirtualHost", "<VirtualHost><Name>test</Name>" + StoreXml(port, host) + "</VirtualHost>", config))
			{
				ADD_FAILURE() << "VirtualHost config did not parse";
				return nullptr;
			}
			return std::make_unique<ocst::VirtualHost>(info::Host("test", "id", config));
		}

		std::shared_ptr<cfg::Server> previous_config;
	};

	TEST_F(OriginAdvertisement, OmittedPortKeepsPublisherBindPort)
	{
		auto host = MakeHost("");
		ASSERT_NE(host, nullptr);
		EXPECT_EQ(host->GetOriginBaseUrl(), "ovt://192.0.2.10:9999");
	}

	TEST_F(OriginAdvertisement, PortChangesOnlyAdvertisedAddressAndIsOwnedByHost)
	{
		auto old_host = MakeHost("<OriginPort>31001</OriginPort>");
		auto new_host = MakeHost("<OriginPort>31002</OriginPort>");
		ASSERT_NE(old_host, nullptr);
		ASSERT_NE(new_host, nullptr);
		EXPECT_EQ(old_host->GetOriginBaseUrl(), "ovt://192.0.2.10:31001");
		EXPECT_EQ(new_host->GetOriginBaseUrl(), "ovt://192.0.2.10:31002");
		new_host.reset();
		EXPECT_EQ(old_host->GetOriginBaseUrl(), "ovt://192.0.2.10:31001");
		EXPECT_EQ(cfg::ConfigManager::GetInstance()->GetServer()->GetBind().GetPublishers().GetOvt().GetPort().GetPort(), 9999);
	}

	TEST_F(OriginAdvertisement, PortWithoutHostKeepsEdgeOnlyMode)
	{
		auto host = MakeHost("<OriginPort>31001</OriginPort>", false);
		ASSERT_NE(host, nullptr);
		EXPECT_TRUE(host->GetOriginBaseUrl().IsEmpty());
	}

	TEST(OriginAdvertisementConfig, AcceptsPortBounds)
	{
		for (int port : {1, 65535})
		{
			cfg::vhost::orgn::OriginMapStore config;
			ASSERT_TRUE(Parse("OriginMapStore", StoreXml("<OriginPort>" + std::to_string(port) + "</OriginPort>"), config));
			EXPECT_EQ(config.GetOriginPort(), port);
		}
	}

	TEST(OriginAdvertisementConfig, RejectsInvalidPorts)
	{
		for (const char *port : {"0", "-1", "65536", "", "not-a-port", "3.1001e4", "65535.5", "31001abc"})
		{
			cfg::vhost::orgn::OriginMapStore config;
			EXPECT_FALSE(Parse("OriginMapStore", StoreXml(std::string("<OriginPort>") + port + "</OriginPort>"), config)) << port;
		}
	}
} // namespace

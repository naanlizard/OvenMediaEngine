#include <gtest/gtest.h>
#include <config/items/items.h>

#define protected public
#include <config/config_manager.h>
#undef protected

#include <base/ovsocket/socket_pool/socket_pool.h>
#include <base/ovsocket/server_socket.h>
#include <modules/containers/bmff/fmp4_packager/duration_boundary_policy.h>
#include <modules/containers/bmff/fmp4_packager/fmp4_storage.h>
#include <modules/http/server/http1/http1_request.h>
#include <modules/http/server/http_connection.h>
#include <modules/http/server/http_exchange.h>
#include "llhls_chunklist.h"
#include "llhls_master_playlist.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <map>
#include <random>
#include <thread>
#include <vector>

#define private public
#include "llhls_session.h"
#include "llhls_stream.h"
#undef private
#include "llhls_application.h"

namespace
{
	class RaceApplicationInfo : public info::Application
	{
	public:
		using info::Application::GetInvalidApplication;
	};

	class RaceRequest : public http::svr::h1::Http1Request
	{
	public:
		RaceRequest(const ov::String &uri, const std::shared_ptr<ov::ClientSocket> &socket) : Http1Request(socket)
		{
			_request_uri = uri;
			_parsed_uri = ov::Url::Parse(uri);
		}
	};

	class RaceResponse : public http::svr::HttpResponse
	{
	public:
		explicit RaceResponse(const std::shared_ptr<ov::ClientSocket> &socket) : HttpResponse(socket) {}
		std::atomic<int> responses{0};

	private:
		int32_t SendHeader() override { return 1; }
		int32_t SendPayload() override
		{
			++responses;
			return 1;
		}
	};

	class RaceConnection : public http::svr::HttpConnection
	{
	public:
		explicit RaceConnection(const std::shared_ptr<ov::ClientSocket> &socket) : HttpConnection(nullptr, socket) {}
		ov::String ToString() const override { return "pending request race test"; }
	};

	class RaceExchange : public http::svr::HttpExchange
	{
	public:
		RaceExchange(const ov::String &uri, const std::shared_ptr<ov::ClientSocket> &socket)
			: HttpExchange(std::make_shared<RaceConnection>(socket)), request(std::make_shared<RaceRequest>(uri, socket)), response(std::make_shared<RaceResponse>(socket)) {}
		std::shared_ptr<http::svr::HttpRequest> GetRequest() const override { return request; }
		std::shared_ptr<http::svr::HttpResponse> GetResponse() const override { return response; }
		std::shared_ptr<RaceRequest> request;
		std::shared_ptr<RaceResponse> response;
	};

	// Real storage, chunklists and an origin-mode session, which every viewer of a stream shares on an edge
	class LLHlsPendingRequestRace : public testing::Test
	{
	protected:
		void SetUp() override
		{
			previous_config = cfg::ConfigManager::GetInstance()->_server;
			cfg::ConfigManager::GetInstance()->_server = std::make_shared<cfg::Server>();
			pool = ov::SocketPool::Create("llhls-race-test", ov::SocketType::Tcp, false);
			ASSERT_TRUE(pool->Initialize(1));
			server_socket = pool->AllocSocket<ov::ServerSocket>(ov::SocketFamily::Inet, pool);
			ASSERT_NE(server_socket, nullptr);
			const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
			ASSERT_GE(fd, 0);
			client_socket = pool->AllocSocket<ov::ClientSocket>(ov::SocketFamily::Inet, server_socket,
				ov::SocketWrapper(ov::SocketType::Tcp, fd), ov::SocketAddress::CreateAndGetFirst("127.0.0.1", 0));
			if (client_socket == nullptr) ::close(fd);
			ASSERT_NE(client_socket, nullptr);
			auto app_info = RaceApplicationInfo::GetInvalidApplication();
			app = std::make_shared<LLHlsApplication>(nullptr, app_info);
			stream = std::make_shared<LLHlsStream>(app, info::Stream(app_info, StreamSourceType::Ovt), true, 0);
			stream->SetState(pub::Stream::State::STARTED);
			for (int id : {1, 2})
			{
				auto track = std::make_shared<MediaTrack>();
				track->SetId(id);
				track->SetMediaType(id == 1 ? cmn::MediaType::Video : cmn::MediaType::Audio);
				track->SetTimeBase(1, 1000);
				stream->AddTrack(track);
				auto policy = std::make_shared<bmff::DurationBoundaryPolicy>(bmff::SegmentBoundaryPolicy::Config{}, false);
				storage[id] = std::make_shared<bmff::FMP4Storage>(nullptr, track, bmff::FMP4Storage::Config{}, "test", policy);
				stream->_storage_map[id] = storage[id];
				stream->_chunklist_map[id] = std::make_shared<LLHlsChunklist>("chunklist.m3u8", track, 10, 6, 0.5, "init.m4s", true);
			}
			stream->_master_playlists["llhls.m3u8"] = std::make_shared<LLHlsMasterPlaylist>();
			session = LLHlsSession::Create(1, true, "test", app, stream, 0);
			ASSERT_NE(session, nullptr);
		}

		void TearDown() override
		{
			session.reset();
			stream.reset();
			app.reset();
			if (client_socket) pool->ReleaseSocket(client_socket);
			if (server_socket) pool->ReleaseSocket(server_socket);
			if (pool) pool->Uninitialize();
			cfg::ConfigManager::GetInstance()->_server = previous_config;
		}

		// A 500 ms part, as the media thread appends it, followed by the playlist update
		void Append(int id, bool complete)
		{
			auto data = ov::String("media").ToData(false);
			ASSERT_TRUE(storage[id]->AppendMediaChunk(data, timestamps[id], 500, true, complete));
			timestamps[id] += 500;
			auto msn = storage[id]->GetLastSegmentNumber() - (complete ? 1 : 0);
			auto segment = std::static_pointer_cast<bmff::FMP4Segment>(storage[id]->GetSegment(msn));
			auto part = segment->GetLastPartialNumber();
			auto writer = stream->_chunklist_map.at(id);
			if (part == 0)
			{
				writer->CreateSegmentInfo(LLHlsChunklist::SegmentInfo(msn, "segment.m4s"));
			}
			writer->AppendPartialSegmentInfo(msn, LLHlsChunklist::SegmentInfo(part, timestamps[id], 0.5, 5, "part.m4s", "next.m4s", true, complete));
			stream->CheckPlaylistReady();
			session->OnPlaylistUpdated(id, msn, part);
		}

		std::shared_ptr<RaceExchange> Exchange(const ov::String &file)
		{
			return std::make_shared<RaceExchange>(ov::String("http://localhost/app/stream/") + file, client_socket);
		}

		std::shared_ptr<cfg::Server> previous_config;
		std::shared_ptr<ov::SocketPool> pool;
		std::shared_ptr<ov::ServerSocket> server_socket;
		std::shared_ptr<ov::ClientSocket> client_socket;
		std::shared_ptr<LLHlsApplication> app;
		std::shared_ptr<LLHlsStream> stream;
		std::shared_ptr<LLHlsSession> session;
		std::map<int, std::shared_ptr<bmff::FMP4Storage>> storage;
		std::map<int, int64_t> timestamps;
	};

	// With StreamWorkerCount 0, request threads park blocking reloads while the stream's media thread answers them
	TEST_F(LLHlsPendingRequestRace, ConcurrentParkAndWakeAnswersEveryRequestOnce)
	{
		constexpr int kClients = 16;
		constexpr auto kRunTime = std::chrono::milliseconds(1500);
		constexpr auto kPartInterval = std::chrono::microseconds(4000);
		// Only keeps a client's loop moving; the verdict is taken after the drain
		constexpr int64_t kGiveUpUpdates = 8;
		constexpr int kDrainUpdates = 10;

		// One part on each track, four parts per segment; used by one thread at a time
		std::map<int, int> parts;
		auto update = [&]() {
			for (int id : {1, 2})
			{
				Append(id, ++parts[id] % 4 == 0);
			}
		};
		for (int n = 0; n < 4; ++n)
		{
			update();
		}
		ASSERT_TRUE(stream->IsReadyToPlay());

		std::atomic<bool> clients_stop{false};
		std::atomic<bool> media_stop{false};
		std::atomic<int64_t> updates{0};
		std::thread media([&]() {
			while (media_stop.load() == false)
			{
				update();
				updates.fetch_add(1);
				std::this_thread::sleep_for(kPartInterval);
			}
		});

		std::vector<std::vector<std::shared_ptr<RaceExchange>>> issued(kClients);
		std::vector<std::thread> clients;
		for (int c = 0; c < kClients; ++c)
		{
			clients.emplace_back([&, c]() {
				std::mt19937 rng(static_cast<uint32_t>(c) + 1u);
				const int track = (c % 2 == 0) ? 1 : 2;
				auto writer = stream->_chunklist_map.at(track);
				while (clients_stop.load() == false)
				{
					int64_t msn = 0, part = 0;
					writer->GetLastSequenceNumber(msn, part);
					// After a completed segment this is answered by the next segment's first part
					auto exchange = Exchange(ov::String::FormatString("chunklist_%d_%s_key_llhls.m3u8?_HLS_msn=%" PRId64 "&_HLS_part=%" PRId64,
																	   track, (track == 1) ? "video" : "audio", msn, part + 1));
					issued[c].push_back(exchange);
					const auto issued_at = updates.load();
					session->OnMessageReceived(std::make_any<std::shared_ptr<http::svr::HttpExchange>>(exchange));
					while ((clients_stop.load() == false) && (exchange->response->responses == 0) && (updates.load() < issued_at + kGiveUpUpdates))
					{
						std::this_thread::sleep_for(std::chrono::microseconds(50));
					}
					std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
				}
			});
		}

		std::this_thread::sleep_for(kRunTime);
		clients_stop.store(true);
		for (auto &client : clients)
		{
			client.join();
		}
		media_stop.store(true);
		media.join();
		for (int n = 0; n < kDrainUpdates; ++n)
		{
			update();
		}

		size_t total = 0, unanswered = 0, answered_twice = 0;
		for (const auto &exchanges : issued)
		{
			for (const auto &exchange : exchanges)
			{
				const int responses = exchange->response->responses;
				total++;
				unanswered += (responses == 0) ? 1 : 0;
				answered_twice += (responses > 1) ? 1 : 0;
			}
		}
		EXPECT_GT(total, 0U);
		EXPECT_EQ(unanswered, 0U) << "of " << total << " requests";
		EXPECT_EQ(answered_twice, 0U);

		// Every request was answered, so the list must be empty, and consistent as the wake loop walks it
		auto &pending = session->_pending_requests;
		size_t walked = 0;
		for (auto it = pending.begin(); (it != pending.end()) && (walked <= total); ++it)
		{
			walked++;
		}
		EXPECT_EQ(walked, 0U);
		EXPECT_EQ(pending.size(), 0U);
	}
}  // namespace

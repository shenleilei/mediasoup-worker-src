#include "ChannelMessageRegistrator.hpp"
#include "DepLibUV.hpp"
#include "FBS/transport.h"
#include "FBS/worker.h"
#include "MediaSoupErrors.hpp"
#include "RTC/Shared.hpp"
#include "RTC/WebRtcServer.hpp"
#include "handles/TcpServerHandle.hpp"
#include "handles/UdpSocketHandle.hpp"
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace
{
	uint16_t PickFreeUdpPort()
	{
		const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

		if (fd < 0)
		{
			throw std::runtime_error("socket() failed");
		}

		struct sockaddr_in address
		{
		};
		address.sin_family      = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port        = 0;

		if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
		{
			::close(fd);
			throw std::runtime_error("bind() failed");
		}

		socklen_t addressLen = sizeof(address);

		if (
		  ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), std::addressof(addressLen)) !=
		  0)
		{
			::close(fd);
			throw std::runtime_error("getsockname() failed");
		}

		::close(fd);

		return ntohs(address.sin_port);
	}

	uint16_t PickFreeTcpPort()
	{
		const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

		if (fd < 0)
		{
			throw std::runtime_error("socket() failed");
		}

		struct sockaddr_in address
		{
		};
		address.sin_family      = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port        = 0;

		if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
		{
			::close(fd);
			throw std::runtime_error("bind() failed");
		}

		socklen_t addressLen = sizeof(address);
		if (
		  ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), std::addressof(addressLen)) !=
		  0)
		{
			::close(fd);
			throw std::runtime_error("getsockname() failed");
		}

		::close(fd);

		return ntohs(address.sin_port);
	}

	size_t CountLoopHandles()
	{
		size_t count{ 0u };

		uv_walk(
		  DepLibUV::GetLoop(),
		  [](uv_handle_t* /*handle*/, void* context)
		  {
			  ++(*static_cast<size_t*>(context));
		  },
		  &count);

		return count;
	}

	void DrainClosingHandles()
	{
		// A handle deleted by a constructor rollback is finalized by libuv on the
		// next nonblocking loop turn. A few bounded turns also keep this helper safe
		// if libuv schedules an intermediate callback.
		for (size_t idx{ 0u }; idx < 4u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
	}

	const FBS::Worker::CreateWebRtcServerRequest* BuildRequest(
	  flatbuffers::FlatBufferBuilder& builder, uint16_t port)
	{
		auto portRange = FBS::Transport::CreatePortRange(builder);
		auto flags     = FBS::Transport::CreateSocketFlags(builder);
		auto listenInfo = FBS::Transport::CreateListenInfoDirect(
		  builder,
		  FBS::Transport::Protocol::UDP,
		  "127.0.0.1",
		  nullptr,
		  port,
		  portRange,
		  flags);
		std::vector<flatbuffers::Offset<FBS::Transport::ListenInfo>> listenInfos{ listenInfo };
		auto request = FBS::Worker::CreateCreateWebRtcServerRequestDirect(
		  builder, "web-rtc-server-construction", &listenInfos);
		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Worker::CreateWebRtcServerRequest>(
		  builder.GetBufferPointer());
	}

	const FBS::Worker::CreateWebRtcServerRequest* BuildRangeRequest(
	  flatbuffers::FlatBufferBuilder& builder,
	  FBS::Transport::Protocol protocol,
	  uint16_t minPort,
	  uint16_t maxPort)
	{
		auto portRange = FBS::Transport::CreatePortRange(builder, minPort, maxPort);
		auto flags     = FBS::Transport::CreateSocketFlags(builder);
		auto listenInfo = FBS::Transport::CreateListenInfoDirect(
		  builder,
		  protocol,
		  "127.0.0.1",
		  nullptr,
		  0u,
		  portRange,
		  flags);
		std::vector<flatbuffers::Offset<FBS::Transport::ListenInfo>> listenInfos{ listenInfo };
		auto request = FBS::Worker::CreateCreateWebRtcServerRequestDirect(
		  builder, "web-rtc-server-range-construction", &listenInfos);
		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Worker::CreateWebRtcServerRequest>(
		  builder.GetBufferPointer());
	}
} // namespace

TEST_CASE(
  "WebRtcServer construction rollback releases unpublished and published sockets",
  "[webrtcserver][oom][memory]")
{
	flatbuffers::FlatBufferBuilder builder;
	const auto port     = PickFreeUdpPort();
	const auto* request = BuildRequest(builder, port);
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);

	for (const auto failurePoint : {
	       RTC::WebRtcServer::ConstructionFailurePointForTesting::BEFORE_SOCKET_PUBLICATION,
	       RTC::WebRtcServer::ConstructionFailurePointForTesting::AFTER_SOCKET_PUBLICATION })
	{
		DYNAMIC_SECTION("failure point " << static_cast<int>(failurePoint))
		{
			// Other Catch2 cases may have scheduled unrelated libuv close
			// callbacks. Drain those before taking this case's handle baseline so
			// random test order cannot be mistaken for a WebRtcServer leak.
			DrainClosingHandles();
			const auto baselineHandleCount = CountLoopHandles();

			RTC::WebRtcServer::SetConstructionFailurePointForTesting(failurePoint);

			CHECK_THROWS_AS(
			  RTC::WebRtcServer(&shared, "web-rtc-server-construction", request->listenInfos()),
			  std::bad_alloc);

			DrainClosingHandles();

			CHECK(CountLoopHandles() == baselineHandleCount);
			CHECK(
			  shared.channelMessageRegistrator->GetChannelRequestHandler(
			    "web-rtc-server-construction") == nullptr);

			// Rebinding the exact port proves the rolled-back listener did not retain
			// its FD. Successful construction also proves handler publication remains
			// usable after the injected failure.
			{
				RTC::WebRtcServer server(
				  &shared, "web-rtc-server-construction", request->listenInfos());
				CHECK(
				  shared.channelMessageRegistrator->GetChannelRequestHandler(
				    "web-rtc-server-construction") == &server);
			}

			DrainClosingHandles();
			CHECK(CountLoopHandles() == baselineHandleCount);
		}
	}
}

#ifdef MS_LIBURING_SUPPORTED
TEST_CASE(
  "WebRtcServer range reservation rolls back when UDP base construction fails",
  "[webrtcserver][liburing][memory]")
{
	flatbuffers::FlatBufferBuilder builder;
	const auto port = PickFreeUdpPort();
	const auto* request = BuildRangeRequest(
	  builder, FBS::Transport::Protocol::UDP, port, port);
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);

	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	UdpSocketHandle::FailNextFilenoForTesting();

	CHECK_THROWS_AS(
	  RTC::WebRtcServer(
	    &shared, "web-rtc-server-range-construction", request->listenInfos()),
	  MediaSoupError);
	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);

	// Reuse the exact one-port logical range. This fails deterministically if
	// PortManager kept the failed construction marked as occupied.
	{
		RTC::WebRtcServer server(
		  &shared, "web-rtc-server-range-construction", request->listenInfos());
		CHECK(
		  shared.channelMessageRegistrator->GetChannelRequestHandler(
		    "web-rtc-server-range-construction") == &server);
	}

	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);
}
#endif

TEST_CASE(
  "WebRtcServer range reservation rolls back when TCP base construction fails",
  "[webrtcserver][memory]")
{
	flatbuffers::FlatBufferBuilder builder;
	const auto port = PickFreeTcpPort();
	const auto* request = BuildRangeRequest(
	  builder, FBS::Transport::Protocol::TCP, port, port);
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);

	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	TcpServerHandle::FailNextLocalAddressForTesting();

	CHECK_THROWS_AS(
	  RTC::WebRtcServer(
	    &shared, "web-rtc-server-range-construction", request->listenInfos()),
	  MediaSoupError);
	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);

	{
		RTC::WebRtcServer server(
		  &shared, "web-rtc-server-range-construction", request->listenInfos());
		CHECK(
		  shared.channelMessageRegistrator->GetChannelRequestHandler(
		    "web-rtc-server-range-construction") == &server);
	}

	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);
}

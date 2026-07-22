#include "ChannelMessageRegistrator.hpp"
#include "DepLibUV.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include "FBS/transport.h"
#include "FBS/worker.h"
#include "RTC/PortManager.hpp"
#include "RTC/ProxyWorkerIpc.hpp"
#include "RTC/ProxyWorkerSocket.hpp"
#include "RTC/Shared.hpp"
#include "RTC/WebRtcServer.hpp"
#include "handles/TcpServerHandle.hpp"
#include "handles/UdpSocketHandle.hpp"
#include <arpa/inet.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace
{
	class ReceiveBatchTestUdpSocketHandle final : public UdpSocketHandle
	{
	public:
		using SendCallback = UdpSocketHandle::onSendCallback;

		explicit ReceiveBatchTestUdpSocketHandle(uv_udp_t* handle) : UdpSocketHandle(handle)
		{
		}

	private:
		void UserOnUdpDatagramReceived(
		  const uint8_t* /*data*/, size_t /*len*/, const struct sockaddr* /*addr*/) override
		{
		}
	};

	class ScopedEnvVar
	{
	public:
		ScopedEnvVar(const char* name, const char* value) : name(name)
		{
			if (const char* previousValue = std::getenv(name))
			{
				this->previous = previousValue;
			}

			REQUIRE(::setenv(name, value, 1) == 0);
		}

		~ScopedEnvVar()
		{
			if (this->previous)
			{
				(void)::setenv(this->name.c_str(), this->previous->c_str(), 1);
			}
			else
			{
				(void)::unsetenv(this->name.c_str());
			}
		}

	private:
		std::string name;
		std::optional<std::string> previous;
	};

	uint16_t PickFreeUdpPort()
	{
		const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

		if (fd < 0)
		{
			throw std::runtime_error("socket() failed");
		}

		struct sockaddr_in address{};
		address.sin_family      = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port        = 0;

		if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
		{
			::close(fd);
			throw std::runtime_error("bind() failed");
		}

		socklen_t addressLen = sizeof(address);

		if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), std::addressof(addressLen)) != 0)
		{
			::close(fd);
			throw std::runtime_error("getsockname() failed");
		}

		::close(fd);

		return ntohs(address.sin_port);
	}

	int BindUdpReceiver(sockaddr_in& outAddress)
	{
		const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		if (fd < 0)
		{
			return -1;
		}

		outAddress                 = {};
		outAddress.sin_family      = AF_INET;
		outAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		outAddress.sin_port        = 0;

		if (::bind(fd, reinterpret_cast<const sockaddr*>(&outAddress), sizeof(outAddress)) != 0)
		{
			::close(fd);

			return -1;
		}

		socklen_t addressLen = sizeof(outAddress);

		if (::getsockname(fd, reinterpret_cast<sockaddr*>(&outAddress), std::addressof(addressLen)) != 0)
		{
			::close(fd);

			return -1;
		}

		return fd;
	}

	uint16_t PickFreeTcpPort()
	{
		const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

		if (fd < 0)
		{
			throw std::runtime_error("socket() failed");
		}

		struct sockaddr_in address{};
		address.sin_family      = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port        = 0;

		if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
		{
			::close(fd);
			throw std::runtime_error("bind() failed");
		}

		socklen_t addressLen = sizeof(address);
		if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), std::addressof(addressLen)) != 0)
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
		  [](uv_handle_t* /*handle*/, void* context) { ++(*static_cast<size_t*>(context)); },
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

	struct TempDir
	{
		std::string path;

		TempDir()
		{
			char tmpl[] = "/tmp/worker-uds-test-XXXXXX";
			char* dir   = ::mkdtemp(tmpl);
			REQUIRE(dir != nullptr);
			path = dir;
		}

		~TempDir()
		{
			if (!path.empty())
			{
				(void)::rmdir(path.c_str());
			}
		}
	};

	bool FillUnixAddress(const std::string& path, sockaddr_un& out, socklen_t& outLen)
	{
		if (path.empty() || path.size() >= sizeof(out.sun_path))
		{
			return false;
		}

		out            = {};
		out.sun_family = AF_UNIX;
		std::memcpy(out.sun_path, path.c_str(), path.size() + 1u);
		outLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1u);

		return true;
	}

	int BindUnixDatagramSocket(const std::string& path)
	{
		const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

		if (fd < 0)
		{
			return -1;
		}

		sockaddr_un addr{};
		socklen_t addrLen{ 0u };

		if (!FillUnixAddress(path, addr, addrLen))
		{
			::close(fd);

			return -1;
		}

		(void)::unlink(path.c_str());

		if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), addrLen) != 0)
		{
			::close(fd);
			(void)::unlink(path.c_str());

			return -1;
		}

		return fd;
	}

	void ConnectUnixDatagramSocket(int fd, const std::string& path)
	{
		sockaddr_un addr{};
		socklen_t addrLen{ 0u };

		REQUIRE(FillUnixAddress(path, addr, addrLen));
		REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), addrLen) == 0);
	}

	std::vector<uint8_t> RecvUnixWithTimeout(int fd)
	{
		pollfd pfd{ fd, POLLIN, 0 };

		if (::poll(&pfd, 1, 500) <= 0 || (pfd.revents & POLLIN) == 0)
		{
			return {};
		}

		std::array<uint8_t, 4096u> buffer{};
		const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);

		if (received <= 0)
		{
			return {};
		}

		return std::vector<uint8_t>(
		  buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received));
	}

	std::vector<uint8_t> RecvUdpWithTimeout(int fd)
	{
		pollfd pfd{ fd, POLLIN, 0 };

		if (::poll(&pfd, 1, 500) <= 0 || (pfd.revents & POLLIN) == 0)
		{
			return {};
		}

		std::array<uint8_t, 4096u> buffer{};
		const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);

		if (received <= 0)
		{
			return {};
		}

		return std::vector<uint8_t>(
		  buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received));
	}

	class ProxyWorkerSocketTestListener final : public RTC::ProxyWorkerSocket::Listener
	{
	public:
		void OnProxyWorkerSocketPacketReceived(
		  RTC::ProxyWorkerSocket* socket,
		  const uint8_t* data,
		  size_t len,
		  const struct sockaddr* remoteAddr) override
		{
			++this->count;
			this->payload.assign(data, data + len);
			this->remoteAddr = Utils::IP::CopyAddress(remoteAddr);

			const std::vector<uint8_t> reply{ 'o', 'k' };
			socket->Send(reply.data(), reply.size(), remoteAddr, nullptr);
		}

	public:
		size_t count{ 0u };
		std::vector<uint8_t> payload;
		sockaddr_storage remoteAddr{};
	};

	const FBS::Worker::CreateWebRtcServerRequest* BuildRequest(
	  flatbuffers::FlatBufferBuilder& builder, uint16_t port)
	{
		auto portRange  = FBS::Transport::CreatePortRange(builder);
		auto flags      = FBS::Transport::CreateSocketFlags(builder);
		auto listenInfo = FBS::Transport::CreateListenInfoDirect(
		  builder, FBS::Transport::Protocol::UDP, "127.0.0.1", nullptr, port, portRange, flags);
		std::vector<flatbuffers::Offset<FBS::Transport::ListenInfo>> listenInfos{ listenInfo };
		auto request = FBS::Worker::CreateCreateWebRtcServerRequestDirect(
		  builder, "web-rtc-server-construction", &listenInfos);
		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Worker::CreateWebRtcServerRequest>(builder.GetBufferPointer());
	}

	const FBS::Worker::CreateWebRtcServerRequest* BuildRangeRequest(
	  flatbuffers::FlatBufferBuilder& builder,
	  FBS::Transport::Protocol protocol,
	  uint16_t minPort,
	  uint16_t maxPort)
	{
		auto portRange  = FBS::Transport::CreatePortRange(builder, minPort, maxPort);
		auto flags      = FBS::Transport::CreateSocketFlags(builder);
		auto listenInfo = FBS::Transport::CreateListenInfoDirect(
		  builder, protocol, "127.0.0.1", nullptr, 0u, portRange, flags);
		std::vector<flatbuffers::Offset<FBS::Transport::ListenInfo>> listenInfos{ listenInfo };
		auto request = FBS::Worker::CreateCreateWebRtcServerRequestDirect(
		  builder, "web-rtc-server-range-construction", &listenInfos);
		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Worker::CreateWebRtcServerRequest>(builder.GetBufferPointer());
	}
} // namespace

TEST_CASE("UdpSocketHandle allocates a real recvmmsg receive batch", "[udp][performance]")
{
	std::string ip{ "127.0.0.1" };
	RTC::Transport::SocketFlags flags;
	auto* uvHandle = RTC::PortManager::BindUdp(ip, PickFreeUdpPort(), flags);

	{
		ReceiveBatchTestUdpSocketHandle socket(uvHandle);

#if defined(__linux__) || defined(__FreeBSD__)
		CHECK(uv_udp_using_recvmmsg(uvHandle) == 1);
		CHECK(UdpSocketHandle::GetReadBufferSizeForTesting() == 8u * 65536u);
#else
		CHECK(uv_udp_using_recvmmsg(uvHandle) == 0);
#endif
	}

	DrainClosingHandles();
}

TEST_CASE("UdpSocketHandle batches UDP sends until the libuv check phase", "[udp][performance]")
{
#ifdef __linux__
	ScopedEnvVar sendBatch("MEDIASOUP_WORKER_UDP_SENDMMSG_BATCH_SIZE", "4");

	sockaddr_in receiverAddr{};
	const int receiverFd = BindUdpReceiver(receiverAddr);
	REQUIRE(receiverFd >= 0);

	std::string ip{ "127.0.0.1" };
	RTC::Transport::SocketFlags flags;
	auto* uvHandle = RTC::PortManager::BindUdp(ip, PickFreeUdpPort(), flags);
	std::vector<bool> callbacks;

	{
		ReceiveBatchTestUdpSocketHandle socket(uvHandle);

		const std::array<std::vector<uint8_t>, 3u> payloads{ std::vector<uint8_t>{ 'o', 'n', 'e' },
			                                                   std::vector<uint8_t>{ 't', 'w', 'o' },
			                                                   std::vector<uint8_t>{
			                                                     't', 'h', 'r', 'e', 'e' } };

		for (const auto& payload : payloads)
		{
			socket.Send(
			  payload.data(),
			  payload.size(),
			  reinterpret_cast<const sockaddr*>(std::addressof(receiverAddr)),
			  new ReceiveBatchTestUdpSocketHandle::SendCallback([&callbacks](bool sent)
			                                                    { callbacks.push_back(sent); }));
		}

		CHECK(callbacks.empty());

		for (size_t idx{ 0u }; idx < 8u && callbacks.size() < payloads.size(); ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}

		REQUIRE(callbacks.size() == payloads.size());
		CHECK(callbacks == std::vector<bool>{ true, true, true });

		for (const auto& payload : payloads)
		{
			CHECK(RecvUdpWithTimeout(receiverFd) == payload);
		}
	}

	::close(receiverFd);
	DrainClosingHandles();
#else
	SUCCEED("sendmmsg batching is Linux-only");
#endif
}

TEST_CASE(
  "ProxyWorkerSocket decodes synthetic UDP tuples and replies to proxy peer",
  "[webrtcserver][proxy-worker-uds]")
{
	TempDir dir;
	const auto workerPath = RTC::ProxyWorkerIpc::WorkerSocketPath(dir.path, 8000);
	const auto clientPath = dir.path + "/proxy-client-test.sock";

	sockaddr_in localAddr{};
	localAddr.sin_family      = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	localAddr.sin_port        = htons(8000);

	ProxyWorkerSocketTestListener listener;

	{
		RTC::ProxyWorkerSocket socket(
		  &listener, workerPath, reinterpret_cast<const sockaddr*>(&localAddr));

		const int clientFd = BindUnixDatagramSocket(clientPath);
		REQUIRE(clientFd >= 0);
		ConnectUnixDatagramSocket(clientFd, workerPath);

		sockaddr_in syntheticRemote{};
		syntheticRemote.sin_family      = AF_INET;
		syntheticRemote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		syntheticRemote.sin_port        = htons(12345);
		const std::vector<uint8_t> payload{ 0x80, 0x60, 0x01, 0x02 };
		std::vector<uint8_t> frame;

		REQUIRE(
		  RTC::ProxyWorkerIpc::EncodeFrame(
		    reinterpret_cast<const sockaddr*>(&syntheticRemote), payload.data(), payload.size(), frame));
		REQUIRE(::send(clientFd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));

		for (size_t idx{ 0u }; idx < 8u && listener.count == 0u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}

		CHECK(listener.count == 1u);
		CHECK(listener.payload == payload);
		CHECK(listener.remoteAddr.ss_family == AF_INET);
		const auto* receivedRemote = reinterpret_cast<const sockaddr_in*>(&listener.remoteAddr);
		CHECK(ntohs(receivedRemote->sin_port) == 12345);

		CHECK(RecvUnixWithTimeout(clientFd) == std::vector<uint8_t>{ 'o', 'k' });

		::close(clientFd);
		(void)::unlink(clientPath.c_str());
	}

	DrainClosingHandles();
	CHECK(::access(workerPath.c_str(), F_OK) != 0);
}

TEST_CASE(
  "ProxyWorkerSocket retries transient reverse-path pressure in order",
  "[webrtcserver][proxy-worker-uds][backpressure]")
{
	TempDir dir;
	const auto workerPath = RTC::ProxyWorkerIpc::WorkerSocketPath(dir.path, 8000);
	const auto clientPath = dir.path + "/proxy-client-retry.sock";
	sockaddr_in localAddr{};
	localAddr.sin_family      = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	localAddr.sin_port        = htons(8000);
	sockaddr_in syntheticRemote{};
	syntheticRemote.sin_family      = AF_INET;
	syntheticRemote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	syntheticRemote.sin_port        = htons(12345);

	ProxyWorkerSocketTestListener listener;
	{
		RTC::ProxyWorkerSocket socket(
		  &listener, workerPath, reinterpret_cast<const sockaddr*>(&localAddr));
		const int clientFd = BindUnixDatagramSocket(clientPath);
		REQUIRE(clientFd >= 0);
		ConnectUnixDatagramSocket(clientFd, workerPath);

		std::vector<uint8_t> frame;
		const std::vector<uint8_t> prime{ 0x80, 0x60, 0x01, 0x02 };
		REQUIRE(
		  RTC::ProxyWorkerIpc::EncodeFrame(
		    reinterpret_cast<const sockaddr*>(&syntheticRemote), prime.data(), prime.size(), frame));
		REQUIRE(::send(clientFd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
		for (size_t idx{ 0u }; idx < 8u && listener.count == 0u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
		REQUIRE(listener.count == 1u);
		CHECK(RecvUnixWithTimeout(clientFd) == std::vector<uint8_t>{ 'o', 'k' });

		socket.FailNextSendsForTesting(EAGAIN, 2u);
		std::vector<bool> callbacks;
		const std::vector<uint8_t> first{ 'o', 'n', 'e' };
		const std::vector<uint8_t> second{ 't', 'w', 'o' };
		socket.Send(
		  first.data(),
		  first.size(),
		  reinterpret_cast<const sockaddr*>(&syntheticRemote),
		  new RTC::ProxyWorkerSocket::onSendCallback([&callbacks](bool sent)
		                                             { callbacks.push_back(sent); }));
		socket.Send(
		  second.data(),
		  second.size(),
		  reinterpret_cast<const sockaddr*>(&syntheticRemote),
		  new RTC::ProxyWorkerSocket::onSendCallback([&callbacks](bool sent)
		                                             { callbacks.push_back(sent); }));
		CHECK(socket.GetPendingSendDatagrams() == 2u);

		for (size_t idx{ 0u }; idx < 32u && callbacks.size() < 2u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
		REQUIRE(callbacks == std::vector<bool>{ true, true });
		CHECK(socket.GetPendingSendDatagrams() == 0u);
		CHECK(socket.GetPendingSendBytes() == 0u);
		CHECK(socket.GetTransientSendRetries() == 2u);
		CHECK(RecvUnixWithTimeout(clientFd) == first);
		CHECK(RecvUnixWithTimeout(clientFd) == second);

		::close(clientFd);
		(void)::unlink(clientPath.c_str());
	}
	DrainClosingHandles();
}

TEST_CASE(
  "ProxyWorkerSocket bounds and expires reverse-path backlog explicitly",
  "[webrtcserver][proxy-worker-uds][backpressure]")
{
	TempDir dir;
	const auto workerPath = RTC::ProxyWorkerIpc::WorkerSocketPath(dir.path, 8000);
	const auto clientPath = dir.path + "/proxy-client-bounded.sock";
	sockaddr_in localAddr{};
	localAddr.sin_family      = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	localAddr.sin_port        = htons(8000);
	sockaddr_in syntheticRemote{};
	syntheticRemote.sin_family      = AF_INET;
	syntheticRemote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	syntheticRemote.sin_port        = htons(12345);

	ProxyWorkerSocketTestListener listener;
	{
		RTC::ProxyWorkerSocket socket(
		  &listener, workerPath, reinterpret_cast<const sockaddr*>(&localAddr));
		const int clientFd = BindUnixDatagramSocket(clientPath);
		REQUIRE(clientFd >= 0);
		ConnectUnixDatagramSocket(clientFd, workerPath);
		std::vector<uint8_t> frame;
		const std::vector<uint8_t> prime{ 0x80, 0x60, 0x01, 0x02 };
		REQUIRE(
		  RTC::ProxyWorkerIpc::EncodeFrame(
		    reinterpret_cast<const sockaddr*>(&syntheticRemote), prime.data(), prime.size(), frame));
		REQUIRE(::send(clientFd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
		for (size_t idx{ 0u }; idx < 8u && listener.count == 0u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
		REQUIRE(listener.count == 1u);
		(void)RecvUnixWithTimeout(clientFd);

		socket.SetSendQueueLimitsForTesting(2u, 64u);
		socket.FailNextSendsForTesting(EAGAIN, 16u);
		std::vector<bool> callbacks;
		const std::vector<uint8_t> payload{ 'x' };
		for (size_t idx{ 0u }; idx < 3u; ++idx)
		{
			socket.Send(
			  payload.data(),
			  payload.size(),
			  reinterpret_cast<const sockaddr*>(&syntheticRemote),
			  new RTC::ProxyWorkerSocket::onSendCallback([&callbacks](bool sent)
			                                             { callbacks.push_back(sent); }));
		}
		CHECK(socket.GetPendingSendDatagrams() == 2u);
		CHECK(socket.GetSendQueueFullDrops() == 1u);
		REQUIRE(callbacks == std::vector<bool>{ false });

		socket.SetMaxPendingSendAgeForTesting(0u);
		socket.OnUvPoll(0, UV_WRITABLE);
		CHECK(socket.GetPendingSendDatagrams() == 0u);
		CHECK(socket.GetSendExpiredDrops() == 2u);
		REQUIRE(callbacks == std::vector<bool>{ false, false, false });

		::close(clientFd);
		(void)::unlink(clientPath.c_str());
	}
	DrainClosingHandles();
}

TEST_CASE(
  "ProxyWorkerSocket expiry timer releases a permanently blocked backlog",
  "[webrtcserver][proxy-worker-uds][backpressure]")
{
	TempDir dir;
	const auto workerPath = RTC::ProxyWorkerIpc::WorkerSocketPath(dir.path, 8000);
	const auto clientPath = dir.path + "/proxy-client-expiry-timer.sock";
	sockaddr_in localAddr{};
	localAddr.sin_family      = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	localAddr.sin_port        = htons(8000);
	sockaddr_in syntheticRemote{};
	syntheticRemote.sin_family      = AF_INET;
	syntheticRemote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	syntheticRemote.sin_port        = htons(12345);

	ProxyWorkerSocketTestListener listener;
	{
		RTC::ProxyWorkerSocket socket(
		  &listener, workerPath, reinterpret_cast<const sockaddr*>(&localAddr));
		const int clientFd = BindUnixDatagramSocket(clientPath);
		REQUIRE(clientFd >= 0);
		ConnectUnixDatagramSocket(clientFd, workerPath);
		std::vector<uint8_t> frame;
		const std::vector<uint8_t> prime{ 0x80, 0x60, 0x01, 0x02 };
		REQUIRE(
		  RTC::ProxyWorkerIpc::EncodeFrame(
		    reinterpret_cast<const sockaddr*>(&syntheticRemote), prime.data(), prime.size(), frame));
		REQUIRE(::send(clientFd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
		for (size_t idx{ 0u }; idx < 8u && listener.count == 0u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
		REQUIRE(listener.count == 1u);
		(void)RecvUnixWithTimeout(clientFd);

		socket.SetMaxPendingSendAgeForTesting(10u);
		socket.FailNextSendsForTesting(EAGAIN, 100000u);
		std::vector<bool> callbacks;
		const std::vector<uint8_t> payload{ 'x' };
		socket.Send(
		  payload.data(),
		  payload.size(),
		  reinterpret_cast<const sockaddr*>(&syntheticRemote),
		  new RTC::ProxyWorkerSocket::onSendCallback([&callbacks](bool sent)
		                                             { callbacks.push_back(sent); }));
		REQUIRE(socket.GetPendingSendDatagrams() == 1u);

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (callbacks.empty() && std::chrono::steady_clock::now() < deadline)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(callbacks == std::vector<bool>{ false });
		CHECK(socket.GetPendingSendDatagrams() == 0u);
		CHECK(socket.GetPendingSendBytes() == 0u);
		CHECK(socket.GetSendExpiredDrops() == 1u);

		::close(clientFd);
		(void)::unlink(clientPath.c_str());
	}
	DrainClosingHandles();
}

TEST_CASE(
  "ProxyWorkerSocket yields receive processing at the configured datagram budget",
  "[webrtcserver][proxy-worker-uds][fairness]")
{
	TempDir dir;
	const auto workerPath = RTC::ProxyWorkerIpc::WorkerSocketPath(dir.path, 8000);
	const auto clientPath = dir.path + "/proxy-client-budget.sock";
	sockaddr_in localAddr{};
	localAddr.sin_family      = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	localAddr.sin_port        = htons(8000);
	sockaddr_in syntheticRemote{};
	syntheticRemote.sin_family      = AF_INET;
	syntheticRemote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	syntheticRemote.sin_port        = htons(12345);

	ProxyWorkerSocketTestListener listener;
	{
		RTC::ProxyWorkerSocket socket(
		  &listener, workerPath, reinterpret_cast<const sockaddr*>(&localAddr));
		socket.SetReceiveBudgetForTesting(2u);
		const int clientFd = BindUnixDatagramSocket(clientPath);
		REQUIRE(clientFd >= 0);
		ConnectUnixDatagramSocket(clientFd, workerPath);
		const std::vector<uint8_t> payload{ 0x80, 0x60, 0x01, 0x02 };
		std::vector<uint8_t> frame;
		REQUIRE(
		  RTC::ProxyWorkerIpc::EncodeFrame(
		    reinterpret_cast<const sockaddr*>(&syntheticRemote), payload.data(), payload.size(), frame));
		for (size_t idx{ 0u }; idx < 5u; ++idx)
		{
			REQUIRE(::send(clientFd, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
		}

		socket.OnUvPoll(0, UV_READABLE);
		CHECK(listener.count == 2u);
		CHECK(socket.GetReceiveBudgetYields() == 1u);
		for (size_t idx{ 0u }; idx < 16u && listener.count < 5u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
		CHECK(listener.count == 5u);
		CHECK(socket.GetReceiveBudgetYields() >= 2u);

		::close(clientFd);
		(void)::unlink(clientPath.c_str());
	}
	DrainClosingHandles();
}

TEST_CASE(
  "WebRtcServer construction rollback releases unpublished and published sockets",
  "[webrtcserver][oom][memory]")
{
	flatbuffers::FlatBufferBuilder builder;
	const auto port     = PickFreeUdpPort();
	const auto* request = BuildRequest(builder, port);
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);

	for (const auto failurePoint :
	     { RTC::WebRtcServer::ConstructionFailurePointForTesting::BEFORE_SOCKET_PUBLICATION,
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
			  shared.channelMessageRegistrator->GetChannelRequestHandler("web-rtc-server-construction") ==
			  nullptr);

			// Rebinding the exact port proves the rolled-back listener did not retain
			// its FD. Successful construction also proves handler publication remains
			// usable after the injected failure.
			{
				RTC::WebRtcServer server(&shared, "web-rtc-server-construction", request->listenInfos());
				CHECK(
				  shared.channelMessageRegistrator->GetChannelRequestHandler("web-rtc-server-construction") ==
				  &server);
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
	const auto port     = PickFreeUdpPort();
	const auto* request = BuildRangeRequest(builder, FBS::Transport::Protocol::UDP, port, port);
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);

	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	UdpSocketHandle::FailNextFilenoForTesting();

	CHECK_THROWS_AS(
	  RTC::WebRtcServer(&shared, "web-rtc-server-range-construction", request->listenInfos()),
	  MediaSoupError);
	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);

	// Reuse the exact one-port logical range. This fails deterministically if
	// PortManager kept the failed construction marked as occupied.
	{
		RTC::WebRtcServer server(&shared, "web-rtc-server-range-construction", request->listenInfos());
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
	const auto port     = PickFreeTcpPort();
	const auto* request = BuildRangeRequest(builder, FBS::Transport::Protocol::TCP, port, port);
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);

	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	TcpServerHandle::FailNextLocalAddressForTesting();

	CHECK_THROWS_AS(
	  RTC::WebRtcServer(&shared, "web-rtc-server-range-construction", request->listenInfos()),
	  MediaSoupError);
	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);

	{
		RTC::WebRtcServer server(&shared, "web-rtc-server-range-construction", request->listenInfos());
		CHECK(
		  shared.channelMessageRegistrator->GetChannelRequestHandler(
		    "web-rtc-server-range-construction") == &server);
	}

	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);
}

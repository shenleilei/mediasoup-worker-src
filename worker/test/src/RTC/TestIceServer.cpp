#include "RTC/IceServer.hpp"
#include "RTC/TransportTuple.hpp"
#include "DepLibUV.hpp"
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <chrono>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace
{
	class TestIceServerListener final : public RTC::IceServer::Listener
	{
	public:
		void OnIceServerSendStunPacket(
		  const RTC::IceServer* /*iceServer*/,
		  const RTC::StunPacket* /*packet*/,
		  RTC::TransportTuple* /*tuple*/) override
		{
		}

		void OnIceServerLocalUsernameFragmentAdded(
		  const RTC::IceServer* /*iceServer*/, const std::string& /*usernameFragment*/) override
		{
		}

		void OnIceServerLocalUsernameFragmentRemoved(
		  const RTC::IceServer* /*iceServer*/, const std::string& /*usernameFragment*/) override
		{
			++this->usernameRemovedCount;

			if (this->throwOnUsernameRemoved)
			{
				throw std::bad_alloc();
			}
		}

		void OnIceServerTupleAdded(
		  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* /*tuple*/) override
		{
		}

		void OnIceServerTupleRemoved(
		  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* /*tuple*/) override
		{
			++this->tupleRemovedCount;

			if (this->throwOnTupleRemoved)
			{
				throw std::bad_alloc();
			}
		}

		void OnIceServerSelectedTuple(
		  const RTC::IceServer* /*iceServer*/, RTC::TransportTuple* /*tuple*/) override
		{
		}

		void OnIceServerConnected(const RTC::IceServer* /*iceServer*/) override
		{
		}

		void OnIceServerCompleted(const RTC::IceServer* /*iceServer*/) override
		{
		}

		void OnIceServerDisconnected(const RTC::IceServer* /*iceServer*/) override
		{
			++this->disconnectedCount;

			if (this->throwOnDisconnected)
			{
				throw std::bad_alloc();
			}
		}

	public:
		bool throwOnUsernameRemoved{ false };
		bool throwOnTupleRemoved{ false };
		bool throwOnDisconnected{ false };
		size_t usernameRemovedCount{ 0u };
		size_t tupleRemovedCount{ 0u };
		size_t disconnectedCount{ 0u };
	};

	template<typename Predicate>
	bool RunLoopUntil(Predicate&& predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

		while (std::chrono::steady_clock::now() < deadline)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

			if (predicate())
			{
				return true;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		return false;
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
} // namespace

TEST_CASE("ICE tuple removal guard resets after listener failure", "[ice][oom]")
{
	TestIceServerListener listener;
	RTC::IceServer iceServer(&listener, "username", "password", 0u);
	struct sockaddr_in remoteAddress
	{
	};
	REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &remoteAddress.sin_addr) == 1);
	remoteAddress.sin_family = AF_INET;
	remoteAddress.sin_port   = htons(40000u);
	RTC::TransportTuple tuple(
	  static_cast<RTC::UdpSocket*>(nullptr),
	  reinterpret_cast<const struct sockaddr*>(&remoteAddress));

	REQUIRE(iceServer.AddTupleForTesting(&tuple) != nullptr);
	REQUIRE(iceServer.GetTupleCountForTesting() == 1u);
	listener.throwOnTupleRemoved = true;

	CHECK_NOTHROW(iceServer.RemoveTuple(&tuple));
	CHECK(listener.tupleRemovedCount == 1u);
	CHECK(iceServer.GetTupleCountForTesting() == 0u);

	// Re-add and remove the same tuple. A latched isRemovingTuples flag would
	// silently ignore this second removal and leave the tuple retained.
	REQUIRE(iceServer.AddTupleForTesting(&tuple) != nullptr);
	REQUIRE(iceServer.GetTupleCountForTesting() == 1u);
	listener.throwOnTupleRemoved = false;

	CHECK_NOTHROW(iceServer.RemoveTuple(&tuple));
	CHECK(listener.tupleRemovedCount == 2u);
	CHECK(iceServer.GetTupleCountForTesting() == 0u);
}

TEST_CASE("ICE destructor contains listener failures and continues tuple cleanup", "[ice][oom]")
{
	TestIceServerListener listener;
	auto iceServer = std::make_unique<RTC::IceServer>(&listener, "username", "password", 0u);
	struct sockaddr_in remoteAddress
	{
	};
	REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &remoteAddress.sin_addr) == 1);
	remoteAddress.sin_family = AF_INET;
	remoteAddress.sin_port   = htons(40001u);
	RTC::TransportTuple tuple(
	  static_cast<RTC::UdpSocket*>(nullptr),
	  reinterpret_cast<const struct sockaddr*>(&remoteAddress));
	REQUIRE(iceServer->AddTupleForTesting(&tuple) != nullptr);

	listener.throwOnUsernameRemoved = true;
	listener.throwOnTupleRemoved    = true;

	CHECK_NOTHROW(iceServer.reset());
	CHECK(listener.usernameRemovedCount == 1u);
	CHECK(listener.tupleRemovedCount == 1u);
}

TEST_CASE("ICE tuple limit eviction survives listener failure", "[ice][oom]")
{
	TestIceServerListener listener;
	RTC::IceServer iceServer(&listener, "username", "password", 0u);
	std::array<struct sockaddr_in, 9u> remoteAddresses{};
	std::vector<std::unique_ptr<RTC::TransportTuple>> tuples;
	tuples.reserve(remoteAddresses.size());

	listener.throwOnTupleRemoved = true;

	for (size_t i{ 0u }; i < remoteAddresses.size(); ++i)
	{
		auto& remoteAddress       = remoteAddresses[i];
		remoteAddress.sin_family = AF_INET;
		remoteAddress.sin_port   = htons(static_cast<uint16_t>(41000u + i));
		REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &remoteAddress.sin_addr) == 1);

		auto tuple = std::make_unique<RTC::TransportTuple>(
		  static_cast<RTC::UdpSocket*>(nullptr),
		  reinterpret_cast<const struct sockaddr*>(&remoteAddress));
		REQUIRE(iceServer.AddTupleForTesting(tuple.get()) != nullptr);
		tuples.emplace_back(std::move(tuple));
	}

	CHECK(listener.tupleRemovedCount == 1u);
	CHECK(iceServer.GetTupleCountForTesting() == 8u);

	// Removing another retained tuple proves a throwing eviction listener did
	// not leave the re-entrancy guard latched.
	listener.throwOnTupleRemoved = false;
	CHECK_NOTHROW(iceServer.RemoveTuple(tuples[1u].get()));
	CHECK(listener.tupleRemovedCount == 2u);
	CHECK(iceServer.GetTupleCountForTesting() == 7u);
}

TEST_CASE("ICE selected tuple removal stops consent timer before listener failure", "[ice][oom]")
{
	const auto baselineHandleCount = CountLoopHandles();
	TestIceServerListener listener;
	{
		RTC::IceServer iceServer(&listener, "username", "password", 10u);
		struct sockaddr_in remoteAddress
		{
		};
		REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &remoteAddress.sin_addr) == 1);
		remoteAddress.sin_family = AF_INET;
		remoteAddress.sin_port   = htons(42000u);
		RTC::TransportTuple tuple(
		  static_cast<RTC::UdpSocket*>(nullptr),
		  reinterpret_cast<const struct sockaddr*>(&remoteAddress));

		iceServer.StartConsentTimeoutForTesting(&tuple, 60000u);
		REQUIRE(iceServer.GetTupleCountForTesting() == 1u);
		REQUIRE(iceServer.IsConsentCheckRunningForTesting());
		listener.throwOnTupleRemoved = true;
		listener.throwOnDisconnected = true;

		CHECK_NOTHROW(iceServer.RemoveTuple(&tuple));
		CHECK(listener.tupleRemovedCount == 1u);
		CHECK(listener.disconnectedCount == 1u);
		CHECK(iceServer.GetTupleCountForTesting() == 0u);
		CHECK(iceServer.GetSelectedTuple() == nullptr);
		CHECK(iceServer.GetState() == RTC::IceServer::IceState::DISCONNECTED);
		CHECK_FALSE(iceServer.IsConsentCheckRunningForTesting());
	}

	REQUIRE(RunLoopUntil(
	  [baselineHandleCount]() { return CountLoopHandles() == baselineHandleCount; }));
}

TEST_CASE("ICE consent timeout contains listener failures at the libuv boundary", "[ice][oom]")
{
	const auto baselineHandleCount = CountLoopHandles();
	TestIceServerListener listener;
	{
		RTC::IceServer iceServer(&listener, "username", "password", 10u);
		struct sockaddr_in remoteAddress
		{
		};
		REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &remoteAddress.sin_addr) == 1);
		remoteAddress.sin_family = AF_INET;
		remoteAddress.sin_port   = htons(42001u);
		RTC::TransportTuple tuple(
		  static_cast<RTC::UdpSocket*>(nullptr),
		  reinterpret_cast<const struct sockaddr*>(&remoteAddress));

		listener.throwOnTupleRemoved = true;
		listener.throwOnDisconnected = true;
		iceServer.StartConsentTimeoutForTesting(&tuple, 1u);

		REQUIRE(RunLoopUntil([&listener]() { return listener.disconnectedCount == 1u; }));
		CHECK(listener.tupleRemovedCount == 1u);
		CHECK(listener.disconnectedCount == 1u);
		CHECK(iceServer.GetTupleCountForTesting() == 0u);
		CHECK(iceServer.GetSelectedTuple() == nullptr);
		CHECK(iceServer.GetState() == RTC::IceServer::IceState::DISCONNECTED);
		CHECK_FALSE(iceServer.IsConsentCheckRunningForTesting());
	}

	REQUIRE(RunLoopUntil(
	  [baselineHandleCount]() { return CountLoopHandles() == baselineHandleCount; }));
}

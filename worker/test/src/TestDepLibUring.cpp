#include "DepLibUV.hpp"
#include "DepLibUringBufferLimits.hpp"
#include "DepLibUringPolicy.hpp"
#include "handles/TcpConnectionHandle.hpp"
#include <srtp.h>
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

TEST_CASE("io_uring fixed send buffers reject oversized writes", "[liburing][capacity]")
{
	STATIC_REQUIRE(SRTP_MAX_SRTCP_TRAILER_LEN >= SRTP_MAX_TRAILER_LEN);

	CHECK(DepLibUringBufferLimits::CanStoreSendData(DepLibUringBufferLimits::SendBufferSize));
	CHECK_FALSE(DepLibUringBufferLimits::CanStoreSendData(DepLibUringBufferLimits::SendBufferSize + 1u));

	constexpr size_t MaxPlaintext = DepLibUringBufferLimits::SendBufferSize - SRTP_MAX_TRAILER_LEN;
	CHECK(DepLibUringBufferLimits::CanStoreSendDataWithTrailer(MaxPlaintext, SRTP_MAX_TRAILER_LEN));
	CHECK_FALSE(
	  DepLibUringBufferLimits::CanStoreSendDataWithTrailer(MaxPlaintext + 1u, SRTP_MAX_TRAILER_LEN));
	CHECK_FALSE(DepLibUringBufferLimits::CanStoreSendDataWithTrailer(0u, 1501u));
	CHECK_FALSE(
	  DepLibUringBufferLimits::CanStoreSendDataWithTrailer(std::numeric_limits<size_t>::max(), 1u));

	CHECK(DepLibUringBufferLimits::CanStoreWriteData(2u, 1498u, false));
	CHECK_FALSE(DepLibUringBufferLimits::CanStoreWriteData(2u, 1499u, false));
	CHECK(DepLibUringBufferLimits::CanStoreWriteData(1500u, 0u, false));
	CHECK_FALSE(DepLibUringBufferLimits::CanStoreWriteData(1501u, 0u, false));
	CHECK_FALSE(
	  DepLibUringBufferLimits::CanStoreWriteData(std::numeric_limits<size_t>::max(), 1u, false));
	CHECK(DepLibUringBufferLimits::CanStoreWriteData(2u, 1500u, true));
	CHECK_FALSE(DepLibUringBufferLimits::CanStoreWriteData(3u, 1499u, true));
	CHECK_FALSE(DepLibUringBufferLimits::CanStoreWriteData(2u, 1501u, true));

	CHECK(DepLibUringBufferLimits::IsCompleteIoResult(1500, 1500u));
	CHECK_FALSE(DepLibUringBufferLimits::IsCompleteIoResult(1499, 1500u));
	CHECK_FALSE(DepLibUringBufferLimits::IsCompleteIoResult(-1, 1500u));
}

TEST_CASE("io_uring submission policy makes bounded forward progress", "[liburing][policy]")
{
	auto drain = [](std::vector<int> results)
	{
		size_t next{ 0u };

		return DepLibUringPolicy::DrainPendingSubmissions(
		  [&results, &next]() { return next < results.size(); },
		  [&results, &next]() { return results[next++]; });
	};

	SECTION("empty queue is already drained")
	{
		const auto result = drain({});

		CHECK(result.outcome == DepLibUringPolicy::SubmitDrainOutcome::Drained);
		CHECK(result.submitCallCount == 0u);
		CHECK(result.submittedCount == 0u);
	}

	SECTION("partial progress and interrupted calls drain the queue")
	{
		const auto result = drain({ -EINTR, -EINTR, 2, 1 });

		CHECK(result.outcome == DepLibUringPolicy::SubmitDrainOutcome::Drained);
		CHECK(result.submitCallCount == 4u);
		CHECK(result.submittedCount == 3u);
	}

	SECTION("repeated interruption is deferred instead of spinning")
	{
		const auto result = drain({ -EINTR, -EINTR, -EINTR, 1 });

		CHECK(result.outcome == DepLibUringPolicy::SubmitDrainOutcome::RetryDeferred);
		CHECK(result.submitCallCount == DepLibUringPolicy::MaxImmediateInterruptedSubmits);
		CHECK(result.submittedCount == 0u);
	}

	SECTION("temporary and zero-progress results are deferred")
	{
		for (const auto resultCode : { 0, -EAGAIN, -EBUSY })
		{
			const auto result = drain({ resultCode, 1 });

			CHECK(result.outcome == DepLibUringPolicy::SubmitDrainOutcome::RetryDeferred);
			CHECK(result.submitCallCount == 1u);
			CHECK(result.lastResult == resultCode);
		}
	}

	SECTION("permanent errors fail closed")
	{
		const auto result = drain({ -EINVAL, 1 });

		CHECK(result.outcome == DepLibUringPolicy::SubmitDrainOutcome::FailClosed);
		CHECK(result.submitCallCount == 1u);
		CHECK(result.lastResult == -EINVAL);
	}

	SECTION("deferred retry budget and backoff are finite")
	{
		CHECK(DepLibUringPolicy::HasDeferredSubmitRetryBudget(0u));
		CHECK(
		  DepLibUringPolicy::HasDeferredSubmitRetryBudget(
		    DepLibUringPolicy::MaxDeferredSubmitRetries - 1u));
		CHECK_FALSE(
		  DepLibUringPolicy::HasDeferredSubmitRetryBudget(DepLibUringPolicy::MaxDeferredSubmitRetries));

		CHECK(DepLibUringPolicy::DeferredSubmitRetryDelayMs(0u) == 1u);
		CHECK(DepLibUringPolicy::DeferredSubmitRetryDelayMs(1u) == 2u);
		CHECK(DepLibUringPolicy::DeferredSubmitRetryDelayMs(6u) == 64u);
		CHECK(DepLibUringPolicy::DeferredSubmitRetryDelayMs(20u) == 64u);
	}
}

TEST_CASE("io_uring completion policy settles callbacks and slots", "[liburing][policy]")
{
	using DepLibUringPolicy::CompletionCallback;

	SECTION("ordinary completions release their slot and require the exact byte count")
	{
		const auto complete   = DepLibUringPolicy::ClassifyCompletion(false, false, false, 1500, 1500u);
		const auto shortWrite = DepLibUringPolicy::ClassifyCompletion(false, false, false, 1499, 1500u);
		const auto failed = DepLibUringPolicy::ClassifyCompletion(false, false, false, -EPIPE, 1500u);

		CHECK(complete.releaseUserData);
		CHECK(complete.callback == CompletionCallback::Sent);
		CHECK(shortWrite.releaseUserData);
		CHECK(shortWrite.callback == CompletionCallback::Failed);
		CHECK(failed.releaseUserData);
		CHECK(failed.callback == CompletionCallback::Failed);
	}

	SECTION("zero-copy retains its slot until the notification")
	{
		const auto initialSuccess = DepLibUringPolicy::ClassifyCompletion(true, false, true, 1500, 1500u);
		const auto initialFailure = DepLibUringPolicy::ClassifyCompletion(true, false, true, 1499, 1500u);
		const auto notification = DepLibUringPolicy::ClassifyCompletion(true, true, false, 0, 1500u);

		CHECK_FALSE(initialSuccess.releaseUserData);
		CHECK(initialSuccess.callback == CompletionCallback::None);
		CHECK_FALSE(initialFailure.releaseUserData);
		CHECK(initialFailure.callback == CompletionCallback::Failed);
		CHECK(notification.releaseUserData);
		CHECK(notification.callback == CompletionCallback::Sent);
	}

	SECTION("callback ownership is detached, deleted, and exception-safe")
	{
		using Callback = std::function<void(bool)>;
		Callback* callback{ nullptr };
		bool callbackResult{ false };
		unsigned int invocationCount{ 0u };
		std::weak_ptr<int> lifetime;

		{
			auto token = std::make_shared<int>(1);
			lifetime   = token;
			callback   = new Callback(
        [&, token](bool sent)
        {
          CHECK(callback == nullptr);
          callbackResult = sent;
          ++invocationCount;
        });
		}

		CHECK_FALSE(lifetime.expired());
		CHECK(
		  DepLibUringPolicy::SettleCallbackNoThrow(callback, true) ==
		  DepLibUringPolicy::CallbackSettlement::Invoked);
		CHECK(callback == nullptr);
		CHECK(callbackResult);
		CHECK(invocationCount == 1u);
		CHECK(lifetime.expired());

		callback = new Callback(
		  [&invocationCount](bool /*sent*/)
		  {
			  ++invocationCount;
			  throw std::runtime_error("test callback failure");
		  });
		CHECK(
		  DepLibUringPolicy::SettleCallbackNoThrow(callback, false) ==
		  DepLibUringPolicy::CallbackSettlement::Threw);
		CHECK(callback == nullptr);
		CHECK(invocationCount == 2u);
		CHECK(
		  DepLibUringPolicy::SettleCallbackNoThrow(callback, false) ==
		  DepLibUringPolicy::CallbackSettlement::NoCallback);
	}

	SECTION("failed zero-copy completion settles once before its notification")
	{
		using Callback = std::function<void(bool)>;
		bool callbackResult{ true };
		unsigned int invocationCount{ 0u };
		Callback* callback = new Callback(
		  [&](bool sent)
		  {
			  callbackResult = sent;
			  ++invocationCount;
		  });

		const auto initialFailure =
		  DepLibUringPolicy::ClassifyCompletion(true, false, true, -EPIPE, 1500u);
		auto* initialCallback = DepLibUringPolicy::TakeCompletionCallback(initialFailure, callback);
		CHECK(
		  DepLibUringPolicy::SettleCallbackNoThrow(initialCallback, false) ==
		  DepLibUringPolicy::CallbackSettlement::Invoked);
		CHECK(callback == nullptr);
		CHECK_FALSE(callbackResult);
		CHECK(invocationCount == 1u);

		const auto notification    = DepLibUringPolicy::ClassifyCompletion(true, true, false, 0, 1500u);
		auto* notificationCallback = DepLibUringPolicy::TakeCompletionCallback(notification, callback);
		CHECK(
		  DepLibUringPolicy::SettleCallbackNoThrow(notificationCallback, true) ==
		  DepLibUringPolicy::CallbackSettlement::NoCallback);
		CHECK(invocationCount == 1u);
	}
}

namespace
{
	class TestTcpConnection final : public TcpConnectionHandle
	{
	public:
		using SendCallback = onSendCallback;

		TestTcpConnection() : TcpConnectionHandle(1024u)
		{
		}

	private:
		void UserOnTcpConnectionRead() override
		{
		}
	};

	class TestTcpConnectionListener final : public TcpConnectionHandle::Listener
	{
	public:
		void OnTcpConnectionClosed(TcpConnectionHandle* /*connection*/) override
		{
			++this->closedCount;
			if (this->throwOnClose)
			{
				throw std::bad_alloc();
			}
		}

		unsigned int closedCount{ 0u };
		bool throwOnClose{ false };
	};

	int CreateConnectedTcpSocket()
	{
		const int listenerFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

		if (listenerFd < 0)
		{
			throw std::runtime_error("cannot create TCP listener");
		}

		sockaddr_in address{};
		address.sin_family      = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port        = 0;
		if (
		  ::bind(listenerFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
		  ::listen(listenerFd, 1) != 0)
		{
			::close(listenerFd);
			throw std::runtime_error("cannot bind TCP listener");
		}

		socklen_t addressLen = sizeof(address);
		if (::getsockname(listenerFd, reinterpret_cast<sockaddr*>(&address), std::addressof(addressLen)) != 0)
		{
			::close(listenerFd);
			throw std::runtime_error("cannot read TCP listener address");
		}

		const int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (clientFd < 0 || ::connect(clientFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			if (clientFd >= 0)
			{
				::close(clientFd);
			}
			::close(listenerFd);
			throw std::runtime_error("cannot connect TCP client");
		}

		const int acceptedFd = ::accept4(listenerFd, nullptr, nullptr, SOCK_CLOEXEC);
		::close(listenerFd);
		::close(clientFd);

		if (acceptedFd < 0)
		{
			throw std::runtime_error("cannot accept TCP client");
		}

		return acceptedFd;
	}

	void DrainClosingHandles()
	{
		for (size_t idx{ 0u }; idx < 8u; ++idx)
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
		}
	}
} // namespace

TEST_CASE("TCP graceful close releases the uv_tcp_t handle", "[tcp-close][lifecycle]")
{
	TestTcpConnectionListener listener;
	sockaddr_storage localAddress{};
	const auto closeCount = TcpConnectionHandle::GetTcpCloseCountForTesting();
	const int acceptedFd  = CreateConnectedTcpSocket();

	{
		TestTcpConnection connection;
		connection.Setup(&listener, &localAddress, "127.0.0.1", 0u);
		const int openResult = uv_tcp_open(connection.GetUvHandle(), acceptedFd);
		if (openResult != 0)
		{
			::close(acceptedFd);
		}
		REQUIRE(openResult == 0);

		connection.TriggerClose();
		CHECK(connection.IsClosed());
		CHECK(listener.closedCount == 1u);
		DrainClosingHandles();
	}

	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
}

TEST_CASE("TCP close falls back when shutdown request allocation fails", "[tcp-close][oom]")
{
	TestTcpConnectionListener listener;
	sockaddr_storage localAddress{};
	const auto closeCount = TcpConnectionHandle::GetTcpCloseCountForTesting();

	{
		TestTcpConnection connection;
		connection.Setup(&listener, &localAddress, "127.0.0.1", 0u);
		TcpConnectionHandle::FailNextShutdownAllocationForTesting();
		connection.TriggerClose();
		CHECK(connection.IsClosed());
		CHECK(listener.closedCount == 1u);
		DrainClosingHandles();
	}

	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
}

#ifdef MS_LIBURING_SUPPORTED
TEST_CASE("io_uring short TCP writes fail close the connection", "[liburing][capacity]")
{
	SECTION("normal callback")
	{
		TestTcpConnectionListener listener;
		sockaddr_storage localAddress{};
		bool callbackResult{ true };
		TestTcpConnection connection;
		connection.Setup(&listener, &localAddress, "127.0.0.1", 0u);

		auto* callback =
		  new TestTcpConnection::SendCallback([&callbackResult](bool sent) { callbackResult = sent; });
		connection.OnLibUringWrite(false, 100u, callback);

		CHECK_FALSE(callbackResult);
		CHECK(connection.IsClosed());
		CHECK(listener.closedCount == 1u);

		uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	}

	SECTION("throwing send callback still closes")
	{
		TestTcpConnectionListener listener;
		sockaddr_storage localAddress{};
		TestTcpConnection connection;
		connection.Setup(&listener, &localAddress, "127.0.0.1", 0u);
		auto* callback =
		  new TestTcpConnection::SendCallback([](bool /*sent*/) { throw std::bad_alloc(); });

		CHECK_NOTHROW(connection.OnLibUringWrite(false, 100u, callback));
		CHECK(connection.IsClosed());
		CHECK(listener.closedCount == 1u);
		uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	}

	SECTION("throwing close listener is contained")
	{
		TestTcpConnectionListener listener;
		listener.throwOnClose = true;
		sockaddr_storage localAddress{};
		TestTcpConnection connection;
		connection.Setup(&listener, &localAddress, "127.0.0.1", 0u);

		CHECK_NOTHROW(connection.OnLibUringWrite(false, 100u, nullptr));
		CHECK(connection.IsClosed());
		CHECK(listener.closedCount == 1u);
		uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	}
}
#endif

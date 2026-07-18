#include "Channel/ChannelSocket.hpp"
#include "DepLibUV.hpp"
#include "FBS/message.h"
#include "FBS/request.h"
#include "MediaSoupErrors.hpp"
#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
	ChannelReadFreeFn NoMessage(
	  uint8_t** /*message*/,
	  uint32_t* /*messageLen*/,
	  size_t* /*messageCtx*/,
	  const void* /*handle*/,
	  ChannelReadCtx /*ctx*/)
	{
		return nullptr;
	}

	struct WriteState
	{
		size_t writes{ 0u };
	};

	void CountWrite(const uint8_t* /*message*/, uint32_t /*messageLen*/, ChannelWriteCtx ctx)
	{
		auto* state = static_cast<WriteState*>(ctx);

		++state->writes;
	}

	class ThrowingListener final : public Channel::ChannelSocket::Listener
	{
	public:
		void HandleRequest(Channel::ChannelRequest* /*request*/) override
		{
			throw std::bad_alloc();
		}

		void HandleNotification(Channel::ChannelNotification* /*notification*/) override
		{
		}

		void OnChannelClosed(Channel::ChannelSocket* /*channel*/) override
		{
			++this->closeCount;
		}

	public:
		size_t closeCount{ 0u };
	};

	class ThrowingCloseListener final : public Channel::ChannelSocket::Listener
	{
	public:
		void HandleRequest(Channel::ChannelRequest* /*request*/) override
		{
		}

		void HandleNotification(Channel::ChannelNotification* /*notification*/) override
		{
		}

		void OnChannelClosed(Channel::ChannelSocket* /*channel*/) override
		{
			++this->closeCount;
			throw std::bad_alloc();
		}

	public:
		size_t closeCount{ 0u };
	};

	class SocketPair
	{
	public:
		SocketPair()
		{
			int fds[2];

			if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
			{
				throw std::runtime_error("socketpair() failed: " + std::to_string(errno));
			}

			this->localFd = fds[0];
			this->peerFd  = fds[1];
		}

		~SocketPair()
		{
			if (this->localFd >= 0)
			{
				::close(this->localFd);
			}

			if (this->peerFd >= 0)
			{
				::close(this->peerFd);
			}
		}

		int ReleaseLocal()
		{
			const auto fd = this->localFd;

			this->localFd = -1;

			return fd;
		}

		void ClosePeer()
		{
			if (this->peerFd >= 0)
			{
				::close(this->peerFd);
				this->peerFd = -1;
			}
		}

	private:
		int localFd{ -1 };
		int peerFd{ -1 };
	};

	void RunLoopUntilIdle()
	{
		REQUIRE(uv_run(DepLibUV::GetLoop(), UV_RUN_DEFAULT) == 0);
	}
} // namespace

TEST_CASE("ChannelSocket contains request callback allocation failures", "[channel][oom]")
{
	WriteState writeState;
	Channel::ChannelSocket channel(NoMessage, nullptr, CountWrite, &writeState);
	ThrowingListener listener;
	channel.SetListener(&listener);

	flatbuffers::FlatBufferBuilder builder;
	auto request =
	  FBS::Request::CreateRequestDirect(builder, 7u, FBS::Request::Method::WORKER_DUMP, "worker");
	auto message = FBS::Message::CreateMessage(builder, FBS::Message::Body::Request, request.Union());
	builder.Finish(message);

	REQUIRE_NOTHROW(
	  channel.ProcessMessageForTesting(FBS::Message::GetMessage(builder.GetBufferPointer())));
	CHECK(channel.IsClosedForTesting());
	CHECK(listener.closeCount == 1u);
	CHECK(writeState.writes == 1u);
}

TEST_CASE("ChannelSocket rejects an unknown request method without invalid cleanup", "[channel]")
{
	WriteState writeState;
	Channel::ChannelSocket channel(NoMessage, nullptr, CountWrite, &writeState);
	ThrowingListener listener;
	channel.SetListener(&listener);

	flatbuffers::FlatBufferBuilder builder;
	auto request = FBS::Request::CreateRequestDirect(
	  builder, 8u, static_cast<FBS::Request::Method>(255u), "worker");
	auto message = FBS::Message::CreateMessage(builder, FBS::Message::Body::Request, request.Union());
	builder.Finish(message);

	REQUIRE_NOTHROW(
	  channel.ProcessMessageForTesting(FBS::Message::GetMessage(builder.GetBufferPointer())));
	CHECK_FALSE(channel.IsClosedForTesting());
	CHECK(listener.closeCount == 0u);
	CHECK(writeState.writes == 1u);
}

TEST_CASE("ChannelSocket contains consumer peer close failures", "[channel]")
{
	SocketPair consumerPair;
	SocketPair producerPair;
	const auto pipeCloseCount = UnixStreamSocketHandle::GetPipeCloseCountForTesting();

	Channel::ChannelSocket channel(consumerPair.ReleaseLocal(), producerPair.ReleaseLocal());

	SECTION("without a registered listener")
	{
		consumerPair.ClosePeer();
		RunLoopUntilIdle();

		CHECK(channel.IsClosedForTesting());
		CHECK(UnixStreamSocketHandle::GetPipeCloseCountForTesting() == pipeCloseCount + 2u);
	}

	SECTION("when the close listener throws")
	{
		ThrowingCloseListener listener;
		channel.SetListener(&listener);

		consumerPair.ClosePeer();
		RunLoopUntilIdle();

		CHECK(channel.IsClosedForTesting());
		CHECK(listener.closeCount == 1u);
		CHECK(UnixStreamSocketHandle::GetPipeCloseCountForTesting() == pipeCloseCount + 2u);
	}

	SECTION("when the producer shutdown request cannot be allocated")
	{
		ThrowingListener listener;
		channel.SetListener(&listener);
		UnixStreamSocketHandle::FailNextShutdownAllocationForTesting();

		consumerPair.ClosePeer();
		RunLoopUntilIdle();

		CHECK(channel.IsClosedForTesting());
		CHECK(listener.closeCount == 1u);
		CHECK(UnixStreamSocketHandle::GetPipeCloseCountForTesting() == pipeCloseCount + 2u);
	}

	SECTION("when stopping consumer reads reports an error")
	{
		ThrowingListener listener;
		channel.SetListener(&listener);
		UnixStreamSocketHandle::FailNextReadStopForTesting();

		consumerPair.ClosePeer();
		RunLoopUntilIdle();

		CHECK(channel.IsClosedForTesting());
		CHECK(listener.closeCount == 1u);
		CHECK(UnixStreamSocketHandle::GetPipeCloseCountForTesting() == pipeCloseCount + 2u);
	}
}

TEST_CASE("ChannelSocket construction rolls back partially initialized handles", "[channel][oom]")
{
	SECTION("producer construction fails after the consumer is active")
	{
		SocketPair consumerPair;
		const auto pipeCloseCount = UnixStreamSocketHandle::GetPipeCloseCountForTesting();
		const auto consumerFd     = consumerPair.ReleaseLocal();

		REQUIRE_THROWS_AS(Channel::ChannelSocket(consumerFd, -1), MediaSoupError);
		RunLoopUntilIdle();

		// One close belongs to the failed producer base constructor and one to
		// the locally-owned consumer rollback.
		CHECK(UnixStreamSocketHandle::GetPipeCloseCountForTesting() == pipeCloseCount + 2u);
	}

	SECTION("async wakeup admission fails after uv_async_init")
	{
		const auto asyncCloseCount = Channel::ChannelSocket::GetAsyncCloseCountForTesting();
		Channel::ChannelSocket::FailNextAsyncSendForTesting();

		REQUIRE_THROWS_AS(
		  Channel::ChannelSocket(NoMessage, nullptr, CountWrite, nullptr),
		  MediaSoupError);
		RunLoopUntilIdle();

		CHECK(Channel::ChannelSocket::GetAsyncCloseCountForTesting() == asyncCloseCount + 1u);
	}
}

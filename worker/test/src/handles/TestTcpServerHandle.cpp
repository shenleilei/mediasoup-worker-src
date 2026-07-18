#include "DepLibUV.hpp"
#include "handles/TcpConnectionHandle.hpp"
#include "handles/TcpServerHandle.hpp"
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
	void DrainClosingHandles()
	{
		uv_run(DepLibUV::GetLoop(), UV_RUN_DEFAULT);
	}

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

	void DeleteTcpHandle(uv_handle_t* handle)
	{
		delete reinterpret_cast<uv_tcp_t*>(handle);
	}

	uv_tcp_t* CreateBoundTcpHandle()
	{
		auto* handle = new uv_tcp_t;
		const int initResult = uv_tcp_init(DepLibUV::GetLoop(), handle);

		if (initResult != 0)
		{
			delete handle;
			throw std::runtime_error("uv_tcp_init() failed: " + std::string(uv_strerror(initResult)));
		}

		struct sockaddr_in address
		{
		};
		const int addressResult = uv_ip4_addr("127.0.0.1", 0, &address);

		if (addressResult != 0)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(handle), DeleteTcpHandle);
			DrainClosingHandles();
			throw std::runtime_error("uv_ip4_addr() failed");
		}

		const int bindResult =
		  uv_tcp_bind(handle, reinterpret_cast<const struct sockaddr*>(&address), 0u);

		if (bindResult != 0)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(handle), DeleteTcpHandle);
			DrainClosingHandles();
			throw std::runtime_error("uv_tcp_bind() failed: " + std::string(uv_strerror(bindResult)));
		}

		return handle;
	}

	class ClientSocket
	{
	public:
		explicit ClientSocket(uint16_t port)
		{
			this->fd = ::socket(AF_INET, SOCK_STREAM, 0);

			if (this->fd < 0)
			{
				throw std::runtime_error("socket() failed");
			}

			struct sockaddr_in address
			{
			};
			address.sin_family = AF_INET;
			address.sin_port   = htons(port);
			if (::inet_pton(AF_INET, "127.0.0.1", std::addressof(address.sin_addr)) != 1)
			{
				Close();
				throw std::runtime_error("inet_pton() failed");
			}

			if (
			  ::connect(
			    this->fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
			{
				Close();
				throw std::runtime_error("connect() failed");
			}
		}

		~ClientSocket()
		{
			Close();
		}

		void Close()
		{
			if (this->fd >= 0)
			{
				::close(this->fd);
				this->fd = -1;
			}
		}

		void SendByte()
		{
			const uint8_t value{ 0x01u };
			const auto sent = ::send(this->fd, &value, sizeof(value), MSG_NOSIGNAL);

			if (sent != static_cast<ssize_t>(sizeof(value)))
			{
				throw std::runtime_error("send() failed");
			}
		}

		size_t ReceiveAvailable()
		{
			std::array<uint8_t, 64u * 1024u> buffer{};
			size_t receivedBytes{ 0u };

			while (true)
			{
				const auto received =
				  ::recv(this->fd, buffer.data(), buffer.size(), MSG_DONTWAIT);

				if (received > 0)
				{
					receivedBytes += static_cast<size_t>(received);

					continue;
				}

				if (received == 0)
				{
					break;
				}

				if (errno == EINTR)
				{
					continue;
				}

				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}

				throw std::runtime_error("recv() failed");
			}

			return receivedBytes;
		}

	private:
		int fd{ -1 };
	};

	class TestTcpConnection final : public TcpConnectionHandle
	{
	public:
		using SendCallback = TcpConnectionHandle::onSendCallback;

		explicit TestTcpConnection(size_t& destructorCount)
		  : TcpConnectionHandle(1024u), destructorCount(destructorCount)
		{
		}

		~TestTcpConnection() override
		{
			++this->destructorCount;
		}

	public:
		bool throwOnRead{ false };
		bool closeOnRead{ false };
		bool throwAfterCloseOnRead{ false };
		size_t* postCloseReadAccessCount{ nullptr };

	private:
		void UserOnTcpConnectionRead() override
		{
			if (this->closeOnRead)
			{
				TriggerClose();
				++this->postCloseReadMarker;

				if (this->postCloseReadAccessCount)
				{
					++(*this->postCloseReadAccessCount);
				}

				if (this->throwAfterCloseOnRead)
				{
					throw std::bad_alloc();
				}
			}

			if (this->throwOnRead)
			{
				throw std::bad_alloc();
			}
		}

	private:
		size_t& destructorCount;
		size_t postCloseReadMarker{ 0u };
	};

	class TestTcpServer final : public TcpServerHandle
	{
	public:
		TestTcpServer(uv_tcp_t* handle, size_t& connectionDestructorCount)
		  : TcpServerHandle(handle), connectionDestructorCount(connectionDestructorCount)
		{
		}

	public:
		size_t allocationAttempts{ 0u };
		size_t closeCallbackAttempts{ 0u };
		bool throwOnAllocation{ false };
		bool throwOnConnectionRead{ false };
		bool closeOnConnectionRead{ false };
		bool throwAfterCloseOnConnectionRead{ false };
		bool throwOnClose{ false };
		TestTcpConnection* connection{ nullptr };
		size_t postCloseReadAccessCount{ 0u };

	private:
		void UserOnTcpConnectionAlloc() override
		{
			++this->allocationAttempts;

			if (this->throwOnAllocation)
			{
				throw std::bad_alloc();
			}

			auto* candidate      = new TestTcpConnection(this->connectionDestructorCount);
			candidate->throwOnRead = this->throwOnConnectionRead;
			candidate->closeOnRead = this->closeOnConnectionRead;
			candidate->throwAfterCloseOnRead = this->throwAfterCloseOnConnectionRead;
			candidate->postCloseReadAccessCount = std::addressof(this->postCloseReadAccessCount);
			AcceptTcpConnection(candidate);

			this->connection = GetNumConnections() == 0u ? nullptr : candidate;
		}

		void UserOnTcpConnectionClosed(TcpConnectionHandle* /*connection*/) override
		{
			++this->closeCallbackAttempts;
			this->connection = nullptr;

			if (this->throwOnClose)
			{
				throw std::bad_alloc();
			}
		}

	private:
		size_t& connectionDestructorCount;
	};

	class CallbackResource final
	{
	public:
		CallbackResource(
		  size_t& destructorCount,
		  const size_t& connectionDestructorCount,
		  bool& destroyedBeforeConnection)
		  : destructorCount(destructorCount),
		    connectionDestructorCount(connectionDestructorCount),
		    destroyedBeforeConnection(destroyedBeforeConnection)
		{
		}

		~CallbackResource()
		{
			++this->destructorCount;
			this->destroyedBeforeConnection = this->connectionDestructorCount == 0u;
		}

	private:
		size_t& destructorCount;
		const size_t& connectionDestructorCount;
		bool& destroyedBeforeConnection;
	};
} // namespace

TEST_CASE("TCP server connection allocation exceptions stay inside the libuv boundary", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		server.throwOnAllocation = true;
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.allocationAttempts != 0u; }));
		CHECK(server.allocationAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(connectionDestructorCount == 0u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP server construction exception closes the listening handle", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	size_t connectionDestructorCount{ 0u };
	TcpServerHandle::ThrowNextLocalAddressForTesting();

	CHECK_THROWS_AS(
	  TestTcpServer(CreateBoundTcpHandle(), connectionDestructorCount),
	  std::bad_alloc);
	DrainClosingHandles();

	CHECK(connectionDestructorCount == 0u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP server setup failure closes the unpublished client handle", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());
		TcpConnectionHandle::FailNextSetupAfterUvInitForTesting();

		REQUIRE(RunLoopUntil([&server]() { return server.allocationAttempts != 0u; }));
		CHECK(server.allocationAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP server insert failure rolls back an accepted client", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());
		TcpServerHandle::FailNextConnectionInsertForTesting();

		REQUIRE(RunLoopUntil([&server]() { return server.allocationAttempts != 0u; }));
		CHECK(server.allocationAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP server start failure rolls back an accepted unpublished client", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());
		TcpConnectionHandle::FailNextStartAfterReadStartForTesting();

		REQUIRE(RunLoopUntil([&server]() { return server.allocationAttempts != 0u; }));
		CHECK(server.allocationAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP read and close listener exceptions still delete the server-owned client", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		server.throwOnConnectionRead = true;
		server.throwOnClose          = true;
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.connection != nullptr);
		REQUIRE(server.GetNumConnections() == 1u);

		CHECK_NOTHROW(server.connection->OnUvRead(1, nullptr));
		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP read close-then-throw defers deletion until callback unwind", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		server.closeOnConnectionRead           = true;
		server.throwAfterCloseOnConnectionRead = true;
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.GetNumConnections() == 1u);
		client.SendByte();

		REQUIRE(RunLoopUntil(
		  [&connectionDestructorCount]() { return connectionDestructorCount == 1u; }));
		CHECK(server.postCloseReadAccessCount == 1u);
		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP write callback exceptions do not skip close ownership cleanup", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		server.throwOnClose = true;
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.connection != nullptr);
		REQUIRE(server.GetNumConnections() == 1u);

		auto callback = std::make_unique<TestTcpConnection::SendCallback>(
		  [](bool /*sent*/) { throw std::bad_alloc(); });
		CHECK_NOTHROW(server.connection->OnUvWrite(UV_EPIPE, callback.get()));
		callback.reset();

		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP write failure callback cannot reentrantly delete the active handler", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };
	size_t callbackCount{ 0u };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.GetNumConnections() == 1u);
		auto* connection = server.connection;
		auto callback    = std::make_unique<TestTcpConnection::SendCallback>(
		  [connection, &callbackCount](bool sent)
		  {
			  CHECK_FALSE(sent);
			  CHECK(connection->IsClosed());
			  ++callbackCount;
			  connection->TriggerClose();
		  });

		CHECK_NOTHROW(connection->OnUvWrite(UV_EPIPE, callback.get()));
		callback.reset();

		CHECK(callbackCount == 1u);
		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP synchronous write releases callback before reentrant close notification", "[tcp]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };
	size_t callbackCount{ 0u };
	size_t callbackResourceDestructorCount{ 0u };
	bool callbackResourceDestroyedBeforeConnection{ false };
	bool writeReturned{ false };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.GetNumConnections() == 1u);
		auto* connection = server.connection;
		auto callbackResource = std::make_shared<CallbackResource>(
		  callbackResourceDestructorCount,
		  connectionDestructorCount,
		  callbackResourceDestroyedBeforeConnection);
		auto* callback = new TestTcpConnection::SendCallback(
		  [connection, callbackResource, &callbackCount, &writeReturned](bool sent)
		  {
			  CHECK(sent);
			  CHECK_FALSE(writeReturned);
			  ++callbackCount;
			  connection->TriggerClose();
		  });
		callbackResource.reset();
		const uint8_t payload{ 0x5au };

		connection->Write(&payload, sizeof(payload), nullptr, 0u, callback);
		writeReturned = true;

		CHECK(callbackCount == 1u);
		CHECK(callbackResourceDestructorCount == 1u);
		CHECK(callbackResourceDestroyedBeforeConnection);
		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP pending-write allocation failure is fail-closed and releases callback", "[tcp][oom]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };
	size_t callbackCount{ 0u };
	size_t callbackResourceDestructorCount{ 0u };
	bool callbackResourceDestroyedBeforeConnection{ false };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.GetNumConnections() == 1u);
		auto* connection = server.connection;
		auto callbackResource = std::make_shared<CallbackResource>(
		  callbackResourceDestructorCount,
		  connectionDestructorCount,
		  callbackResourceDestroyedBeforeConnection);
		auto* callback = new TestTcpConnection::SendCallback(
		  [connection, callbackResource, &callbackCount](bool sent)
		  {
			  CHECK_FALSE(sent);
			  CHECK(connection->IsClosed());
			  ++callbackCount;
			  connection->TriggerClose();
		  });
		callbackResource.reset();
		const uint8_t payload{ 0x5au };
		TcpConnectionHandle::FailNextWriteDataAllocationForTesting();

		CHECK_NOTHROW(connection->Write(&payload, sizeof(payload), nullptr, 0u, callback));

		CHECK(callbackCount == 1u);
		CHECK(callbackResourceDestructorCount == 1u);
		CHECK(callbackResourceDestroyedBeforeConnection);
		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

TEST_CASE("TCP libuv write callback defers reentrant close until completion unwind", "[tcp]")
{
	DrainClosingHandles();
	const auto baselineHandleCount = CountLoopHandles();
	const auto closeCount          = TcpConnectionHandle::GetTcpCloseCountForTesting();
	size_t connectionDestructorCount{ 0u };
	size_t callbackCount{ 0u };
	bool writeReturned{ false };

	{
		TestTcpServer server(CreateBoundTcpHandle(), connectionDestructorCount);
		ClientSocket client(server.GetLocalPort());

		REQUIRE(RunLoopUntil([&server]() { return server.connection != nullptr; }));
		REQUIRE(server.GetNumConnections() == 1u);
		auto* connection = server.connection;
		int sendBufferSize{ 4096 };
		REQUIRE(
		  uv_send_buffer_size(
		    reinterpret_cast<uv_handle_t*>(connection->GetUvHandle()),
		    std::addressof(sendBufferSize)) == 0);
		std::vector<uint8_t> payload(1024u * 1024u, 0x5au);
		size_t receivedBytes{ 0u };
		auto* callback = new TestTcpConnection::SendCallback(
		  [connection, &callbackCount, &writeReturned](bool sent)
		  {
			  CHECK(sent);
			  CHECK(writeReturned);
			  ++callbackCount;
			  connection->TriggerClose();
		  });

		connection->Write(payload.data(), payload.size(), nullptr, 0u, callback);
		REQUIRE(callbackCount == 0u);
		writeReturned = true;

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

		while (
		  std::chrono::steady_clock::now() < deadline &&
		  (callbackCount != 1u || receivedBytes != payload.size()))
		{
			uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
			receivedBytes += client.ReceiveAvailable();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		REQUIRE(callbackCount == 1u);
		CHECK(receivedBytes == payload.size());
		CHECK(server.closeCallbackAttempts == 1u);
		CHECK(server.GetNumConnections() == 0u);
		CHECK(server.connection == nullptr);
		CHECK(connectionDestructorCount == 1u);
		client.Close();
	}

	DrainClosingHandles();
	CHECK(TcpConnectionHandle::GetTcpCloseCountForTesting() == closeCount + 1u);
	CHECK(CountLoopHandles() == baselineHandleCount);
}

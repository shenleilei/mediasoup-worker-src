#define MS_CLASS "TcpServerHandle"
// #define MS_LOG_DEV_LEVEL 3

#include "handles/TcpServerHandle.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include <exception>
#include <memory>
#include <new>

#define MS_TRACE_NO_THROW() \
	do \
	{ \
		try \
		{ \
			MS_TRACE(); \
		} \
		catch (...) \
		{ \
		} \
	} while (false)

#define MS_ERROR_NO_THROW(...) \
	do \
	{ \
		try \
		{ \
			MS_ERROR(__VA_ARGS__); \
		} \
		catch (...) \
		{ \
		} \
	} while (false)

#define MS_DEBUG_DEV_NO_THROW(...) \
	do \
	{ \
		try \
		{ \
			MS_DEBUG_DEV(__VA_ARGS__); \
		} \
		catch (...) \
		{ \
		} \
	} while (false)

/* Static. */

static constexpr int ListenBacklog{ 512 };

#ifdef MS_TEST
thread_local static bool failNextLocalAddressForTesting{ false };
thread_local static bool throwNextLocalAddressForTesting{ false };
thread_local static bool failNextConnectionInsertForTesting{ false };
#endif

/* Static methods for UV callbacks. */

inline static void onConnection(uv_stream_t* handle, int status) noexcept
{
	auto* server = static_cast<TcpServerHandle*>(handle->data);

	if (server)
	{
		try
		{
			server->OnUvConnection(status);
		}
		catch (const std::exception& error)
		{
			MS_ERROR_NO_THROW("uncaught TCP server connection callback exception: %s", error.what());
		}
		catch (...)
		{
			MS_ERROR_NO_THROW("uncaught unknown TCP server connection callback exception");
		}
	}
}

inline static void onCloseTcp(uv_handle_t* handle)
{
	delete reinterpret_cast<uv_tcp_t*>(handle);
}

/* Instance methods. */

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
TcpServerHandle::TcpServerHandle(uv_tcp_t* uvHandle) : uvHandle(uvHandle)
{
	MS_TRACE_NO_THROW();

	int err;

	this->uvHandle->data = static_cast<void*>(this);

	try
	{
		err = uv_listen(
		  reinterpret_cast<uv_stream_t*>(this->uvHandle),
		  ListenBacklog,
		  static_cast<uv_connection_cb>(onConnection));

		if (err != 0)
		{
			MS_THROW_ERROR("uv_listen() failed: %s", uv_strerror(err));
		}

		// Set local address.
#ifdef MS_TEST
		if (throwNextLocalAddressForTesting)
		{
			throwNextLocalAddressForTesting = false;
			throw std::bad_alloc();
		}

		const bool localAddressSet = failNextLocalAddressForTesting
		                               ? (failNextLocalAddressForTesting = false)
		                               : SetLocalAddress();
#else
		const bool localAddressSet = SetLocalAddress();
#endif
		if (!localAddressSet)
		{
			MS_THROW_ERROR("error setting local IP and port");
		}
	}
	catch (...)
	{
		// A constructor exception skips ~TcpServerHandle(). Detach the libuv
		// callback from the not-fully-constructed object and close the handle here.
		this->uvHandle->data = nullptr;
		uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseTcp));

		throw;
	}
}

#ifdef MS_TEST
void TcpServerHandle::FailNextLocalAddressForTesting()
{
	failNextLocalAddressForTesting = true;
}

void TcpServerHandle::ThrowNextLocalAddressForTesting()
{
	throwNextLocalAddressForTesting = true;
}

void TcpServerHandle::FailNextConnectionInsertForTesting()
{
	failNextConnectionInsertForTesting = true;
}
#endif

TcpServerHandle::~TcpServerHandle()
{
	MS_TRACE();

	if (!this->closed)
	{
		InternalClose();
	}
}

void TcpServerHandle::Dump() const
{
	MS_DUMP("<TcpServerHandle>");
	MS_DUMP("  localIp: %s", this->localIp.c_str());
	MS_DUMP("  localPort: %" PRIu16, static_cast<uint16_t>(this->localPort));
	MS_DUMP("  num connections: %zu", this->connections.size());
	MS_DUMP("  closed: %s", this->closed ? "yes" : "no");
	MS_DUMP("</TcpServerHandle>");
}

uint32_t TcpServerHandle::GetSendBufferSize() const
{
	MS_TRACE();

	int size{ 0 };
	const int err =
	  uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(this->uvHandle), std::addressof(size));

	if (err)
	{
		MS_THROW_ERROR("uv_send_buffer_size() failed: %s", uv_strerror(err));
	}

	return static_cast<uint32_t>(size);
}

void TcpServerHandle::SetSendBufferSize(uint32_t size)
{
	MS_TRACE();

	auto sizeInt = static_cast<int>(size);

	if (sizeInt <= 0)
	{
		MS_THROW_TYPE_ERROR("invalid size: %d", sizeInt);
	}

	const int err =
	  uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(this->uvHandle), std::addressof(sizeInt));

	if (err)
	{
		MS_THROW_ERROR("uv_send_buffer_size() failed: %s", uv_strerror(err));
	}
}

uint32_t TcpServerHandle::GetRecvBufferSize() const
{
	MS_TRACE();

	int size{ 0 };
	const int err =
	  uv_recv_buffer_size(reinterpret_cast<uv_handle_t*>(this->uvHandle), std::addressof(size));

	if (err)
	{
		MS_THROW_ERROR("uv_recv_buffer_size() failed: %s", uv_strerror(err));
	}

	return static_cast<uint32_t>(size);
}

void TcpServerHandle::SetRecvBufferSize(uint32_t size)
{
	MS_TRACE();

	auto sizeInt = static_cast<int>(size);

	if (sizeInt <= 0)
	{
		MS_THROW_TYPE_ERROR("invalid size: %d", sizeInt);
	}

	const int err =
	  uv_recv_buffer_size(reinterpret_cast<uv_handle_t*>(this->uvHandle), std::addressof(sizeInt));

	if (err)
	{
		MS_THROW_ERROR("uv_recv_buffer_size() failed: %s", uv_strerror(err));
	}
}

void TcpServerHandle::AcceptTcpConnection(TcpConnectionHandle* connection) noexcept
{
	MS_TRACE_NO_THROW();

	MS_ASSERT(connection != nullptr, "TcpConnectionHandle pointer was not allocated by the user");
	std::unique_ptr<TcpConnectionHandle> connectionOwner(connection);

	try
	{
		connection->Setup(this, &(this->localAddr), this->localIp, this->localPort);

		// Accept the connection.
		const int err = uv_accept(
		  reinterpret_cast<uv_stream_t*>(this->uvHandle),
		  reinterpret_cast<uv_stream_t*>(connection->GetUvHandle()));

		if (err != 0)
		{
			MS_ERROR_NO_THROW("uv_accept() failed, dropping TCP connection: %s", uv_strerror(err));

			return;
		}

		// Start receiving data.
		// NOTE: This may throw.
		connection->Start();

#ifdef MS_TEST
		if (failNextConnectionInsertForTesting)
		{
			failNextConnectionInsertForTesting = false;
			throw std::bad_alloc();
		}
#endif

		// Publish the connection only after setup, accept, and read startup all
		// succeed. Until then the local owner closes every partially initialized
		// libuv handle on failure.
		const auto insertResult = this->connections.insert(connection);

		if (!insertResult.second)
		{
			// The set already owns this exact pointer. Do not let the local guard
			// delete the published instance if a buggy caller passes it twice.
			connectionOwner.release();
			MS_ERROR_NO_THROW("TCP connection pointer is already owned by the server");

			return;
		}

		connectionOwner.release();
	}
	catch (const MediaSoupError& error)
	{
		MS_ERROR_NO_THROW("cannot accept TCP connection: %s", error.what());
	}
	catch (const std::exception& error)
	{
		MS_ERROR_NO_THROW("cannot accept TCP connection: %s", error.what());
	}
	catch (...)
	{
		MS_ERROR_NO_THROW("cannot accept TCP connection due to an unknown exception");
	}
}

void TcpServerHandle::InternalClose()
{
	MS_TRACE();

	if (this->closed)
	{
		return;
	}

	this->closed = true;

	// Tell the UV handle that the TcpServerHandle has been closed.
	this->uvHandle->data = nullptr;

	MS_DEBUG_DEV("closing %zu active connections", this->connections.size());

	for (auto* connection : this->connections)
	{
		delete connection;
	}

	uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseTcp));
}

bool TcpServerHandle::SetLocalAddress()
{
	MS_TRACE();

	int err;
	int len = sizeof(this->localAddr);

	err =
	  uv_tcp_getsockname(this->uvHandle, reinterpret_cast<struct sockaddr*>(&this->localAddr), &len);

	if (err != 0)
	{
		MS_ERROR("uv_tcp_getsockname() failed: %s", uv_strerror(err));

		return false;
	}

	int family;

	Utils::IP::GetAddressInfo(
	  reinterpret_cast<const struct sockaddr*>(&this->localAddr), family, this->localIp, this->localPort);

	return true;
}

inline void TcpServerHandle::OnUvConnection(int status) noexcept
{
	MS_TRACE_NO_THROW();

	if (this->closed)
	{
		return;
	}

	if (status != 0)
	{
		MS_ERROR_NO_THROW("error while receiving a new TCP connection: %s", uv_strerror(status));

		return;
	}

	// Notify the subclass about a new TCP connection attempt. This method is
	// called by libuv, so no C++ exception may cross the callback boundary.
	try
	{
		UserOnTcpConnectionAlloc();
	}
	catch (const std::exception& error)
	{
		MS_ERROR_NO_THROW("TCP connection allocation callback failed: %s", error.what());
	}
	catch (...)
	{
		MS_ERROR_NO_THROW("TCP connection allocation callback failed with an unknown exception");
	}
}

inline void TcpServerHandle::OnTcpConnectionClosed(TcpConnectionHandle* connection) noexcept
{
	MS_TRACE_NO_THROW();

	MS_DEBUG_DEV_NO_THROW("TCP connection closed");

	// Only the set owns published connections. Refuse duplicate or foreign close
	// notifications instead of risking a second delete.
	if (this->connections.erase(connection) == 0u)
	{
		MS_ERROR_NO_THROW("ignoring close notification for an unowned TCP connection");

		return;
	}

	std::unique_ptr<TcpConnectionHandle> connectionOwner(connection);

	// Notify the subclass.
	try
	{
		UserOnTcpConnectionClosed(connection);
	}
	catch (const std::exception& error)
	{
		MS_ERROR_NO_THROW("TCP connection close callback failed: %s", error.what());
	}
	catch (...)
	{
		MS_ERROR_NO_THROW("TCP connection close callback failed with an unknown exception");
	}
}

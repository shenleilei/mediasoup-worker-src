#define MS_CLASS "TcpConnectionHandle"
// #define MS_LOG_DEV_LEVEL 3

#include "handles/TcpConnectionHandle.hpp"
#include "DepLibUV.hpp"
#ifdef MS_LIBURING_SUPPORTED
#include "DepLibUring.hpp"
#endif
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include <cstring> // std::memcpy()
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

#define MS_WARN_DEV_NO_THROW(...) \
	do \
	{ \
		try \
		{ \
			MS_WARN_DEV(__VA_ARGS__); \
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

#ifdef MS_TEST
namespace
{
	thread_local bool failNextSetupAfterUvInitForTesting{ false };
	thread_local bool failNextStartAfterReadStartForTesting{ false };
	thread_local bool failNextWriteDataAllocationForTesting{ false };
	thread_local bool failNextShutdownAllocationForTesting{ false };
	thread_local size_t tcpCloseCountForTesting{ 0u };
}
#endif

/* Static methods for UV callbacks. */

inline static void notifyConnectionClosedNoThrow(
  TcpConnectionHandle* connection, TcpConnectionHandle::Listener* listener) noexcept
{
	if (!listener)
	{
		return;
	}

	try
	{
		listener->OnTcpConnectionClosed(connection);
	}
	catch (const std::exception& error)
	{
		MS_ERROR_NO_THROW("TCP connection close listener failed: %s", error.what());
	}
	catch (...)
	{
		MS_ERROR_NO_THROW("TCP connection close listener failed with an unknown exception");
	}
}

inline static void onAlloc(uv_handle_t* handle, size_t suggestedSize, uv_buf_t* buf) noexcept
{
	auto* connection = static_cast<TcpConnectionHandle*>(handle->data);

	if (connection)
	{
		try
		{
			connection->OnUvReadAlloc(suggestedSize, buf);
		}
		catch (const std::exception& error)
		{
			MS_ERROR_NO_THROW("uncaught TCP read allocation callback exception: %s", error.what());
			buf->base = nullptr;
			buf->len  = 0u;
		}
		catch (...)
		{
			MS_ERROR_NO_THROW("uncaught unknown TCP read allocation callback exception");
			buf->base = nullptr;
			buf->len  = 0u;
		}
	}
}

inline static void onRead(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) noexcept
{
	auto* connection = static_cast<TcpConnectionHandle*>(handle->data);

	if (connection)
	{
		connection->BeginUvCallback();

		try
		{
			connection->OnUvRead(nread, buf);
		}
		catch (const std::exception& error)
		{
			MS_ERROR_NO_THROW("uncaught TCP read callback exception: %s", error.what());
			connection->ErrorReceiving();
		}
		catch (...)
		{
			MS_ERROR_NO_THROW("uncaught unknown TCP read callback exception");
			connection->ErrorReceiving();
		}

		auto* closeListener = connection->EndUvCallback();
		notifyConnectionClosedNoThrow(connection, closeListener);
	}
}

inline static void onWrite(uv_write_t* req, int status) noexcept
{
	std::unique_ptr<TcpConnectionHandle::UvWriteData> writeData(
	  static_cast<TcpConnectionHandle::UvWriteData*>(req->data));
	auto* handle     = req->handle;
	auto* connection = static_cast<TcpConnectionHandle*>(handle->data);
	auto* cb         = writeData->cb;

	if (connection)
	{
		connection->BeginUvCallback();

		try
		{
			connection->OnUvWrite(status, cb);
		}
		catch (const std::exception& error)
		{
			MS_ERROR_NO_THROW("uncaught TCP write callback exception: %s", error.what());
			connection->ErrorReceiving();
		}
		catch (...)
		{
			MS_ERROR_NO_THROW("uncaught unknown TCP write callback exception");
			connection->ErrorReceiving();
		}

		auto* closeListener = connection->EndUvCallback();
		writeData.reset();
		notifyConnectionClosedNoThrow(connection, closeListener);
	}
}

// NOTE: We have different onCloseXxx() callbacks to avoid an ASAN warning by
// ensuring that we call `delete xxx` with same type as `new xxx` before.
inline static void onCloseTcp(uv_handle_t* handle)
{
#ifdef MS_TEST
	++tcpCloseCountForTesting;
#endif

	delete reinterpret_cast<uv_tcp_t*>(handle);
}

inline static void onShutdown(uv_shutdown_t* req, int /*status*/)
{
	auto* handle = req->handle;

	delete req;

	// Now do close the handle.
	uv_close(reinterpret_cast<uv_handle_t*>(handle), static_cast<uv_close_cb>(onCloseTcp));
}

/* Instance methods. */

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
TcpConnectionHandle::TcpConnectionHandle(size_t bufferSize)
  : bufferSize(bufferSize), uvHandle(new uv_tcp_t)
{
	MS_TRACE_NO_THROW();

	this->uvHandle->data = static_cast<void*>(this);

	// NOTE: Don't allocate the buffer here. Instead wait for the first uv_alloc_cb().
}

TcpConnectionHandle::~TcpConnectionHandle()
{
	MS_TRACE();

	if (!this->closed)
	{
		InternalClose();
	}

	delete[] this->buffer;
}

void TcpConnectionHandle::TriggerClose()
{
	MS_TRACE();

	if (this->closed)
	{
		return;
	}

	InternalClose();

	NotifyClosed();
}

void TcpConnectionHandle::Dump() const
{
	MS_DUMP("<TcpConnectionHandle>");
	MS_DUMP("  localIp: %s", this->localIp.c_str());
	MS_DUMP("  localPort: %" PRIu16, static_cast<uint16_t>(this->localPort));
	MS_DUMP("  remoteIp: %s", this->peerIp.c_str());
	MS_DUMP("  remotePort: %" PRIu16, static_cast<uint16_t>(this->peerPort));
	MS_DUMP("  closed: %s", this->closed ? "yes" : "no");
	MS_DUMP("</TcpConnectionHandle>");
}

void TcpConnectionHandle::Setup(
  Listener* listener, struct sockaddr_storage* localAddr, const std::string& localIp, uint16_t localPort)
{
	MS_TRACE();

	// Set the UV handle.
	const int err = uv_tcp_init(DepLibUV::GetLoop(), this->uvHandle);

	if (err != 0)
	{
		MS_THROW_ERROR("uv_tcp_init() failed: %s", uv_strerror(err));
	}

	this->uvHandleInitialized = true;

#ifdef MS_TEST
	if (failNextSetupAfterUvInitForTesting)
	{
		failNextSetupAfterUvInitForTesting = false;
		throw std::bad_alloc();
	}
#endif

	// Set the listener.
	this->listener = listener;

	// Set the local address.
	this->localAddr = localAddr;
	this->localIp   = localIp;
	this->localPort = localPort;
}

void TcpConnectionHandle::Start()
{
	MS_TRACE();

	if (this->closed)
	{
		return;
	}

	// NOLINTNEXTLINE(misc-const-correctness)
	int err = uv_read_start(
	  reinterpret_cast<uv_stream_t*>(this->uvHandle),
	  static_cast<uv_alloc_cb>(onAlloc),
	  static_cast<uv_read_cb>(onRead));

	if (err != 0)
	{
		MS_THROW_ERROR("uv_read_start() failed: %s", uv_strerror(err));
	}

#ifdef MS_TEST
	if (failNextStartAfterReadStartForTesting)
	{
		failNextStartAfterReadStartForTesting = false;
		throw std::bad_alloc();
	}
#endif

	// Get the peer address.
	if (!SetPeerAddress())
	{
		MS_THROW_ERROR("error setting peer IP and port");
	}

#ifdef MS_LIBURING_SUPPORTED
	err = uv_fileno(reinterpret_cast<uv_handle_t*>(this->uvHandle), std::addressof(this->fd));

	if (err != 0)
	{
		MS_THROW_ERROR("uv_fileno() failed: %s", uv_strerror(err));
	}
#endif
}

void TcpConnectionHandle::Write(
  const uint8_t* data1,
  size_t len1,
  const uint8_t* data2,
  size_t len2,
  TcpConnectionHandle::onSendCallback* cb) noexcept
{
	std::unique_ptr<onSendCallback> cbOwner(cb);

	MS_TRACE_NO_THROW();

	if (this->closed)
	{
		auto* closeListener = InvokeSendCallbackNoThrow(cbOwner.get(), false);
		cbOwner.reset();
		notifyConnectionClosedNoThrow(this, closeListener);

		return;
	}

	if (len1 == 0 && len2 == 0)
	{
		auto* closeListener = InvokeSendCallbackNoThrow(cbOwner.get(), false);
		cbOwner.reset();
		notifyConnectionClosedNoThrow(this, closeListener);

		return;
	}

#ifdef MS_LIBURING_SUPPORTED
	{
		if (!DepLibUring::IsActive())
		{
			goto write_libuv;
		}

		// Prepare the data to be sent.
		// NOTE: If all SQEs are currently in use or no UserData entry is available we'll
		// fall back to libuv.
		struct CallbackHolder
		{
			~CallbackHolder()
			{
				delete this->cb;
			}

			onSendCallback* Release()
			{
				auto* cb = this->cb;
				this->cb = nullptr;

				return cb;
			}

			onSendCallback* cb{ nullptr };
		};

		std::shared_ptr<CallbackHolder> cbHolder;

		try
		{
			cbHolder                  = std::make_shared<CallbackHolder>();
			cbHolder->cb              = cbOwner.release();
			const auto lifetimeToken  = std::weak_ptr<uint8_t>(this->liburingLifetimeToken);
			const auto totalLen       = len1 + len2;
			auto liburingCb           = std::make_unique<DepLibUring::onSendCallback>(
			  [this, lifetimeToken, totalLen, cbHolder](bool sent)
			  {
				  auto* cb      = cbHolder->Release();
				  auto lifetime = lifetimeToken.lock();

				  if (!lifetime)
				  {
					  std::unique_ptr<onSendCallback> cbOwner(cb);

					  if (cb)
					  {
						  try
						  {
							  (*cb)(false);
						  }
						  catch (const std::exception& error)
						  {
							  MS_ERROR_NO_THROW(
							    "TCP io_uring expired-lifetime callback failed: %s", error.what());
						  }
						  catch (...)
						  {
							  MS_ERROR_NO_THROW(
							    "TCP io_uring expired-lifetime callback failed with an unknown exception");
						  }
					  }

					  return;
				  }

				  OnLibUringWrite(sent, totalLen, cb);
			  });
			const bool prepared =
			  DepLibUring::PrepareWrite(this->fd, data1, len1, data2, len2, liburingCb.get());

			if (!prepared)
			{
				MS_DEBUG_DEV_NO_THROW("cannot write via liburing, fallback to libuv");
				cbOwner.reset(cbHolder->Release());

				goto write_libuv;
			}

			liburingCb.release();

			return;
		}
		catch (const std::exception& error)
		{
			if (cbHolder && !cbOwner)
			{
				cbOwner.reset(cbHolder->Release());
			}

			MS_ERROR_NO_THROW("cannot prepare TCP io_uring write: %s", error.what());
			HandleWriteFailure(cbOwner.release());

			return;
		}
		catch (...)
		{
			if (cbHolder && !cbOwner)
			{
				cbOwner.reset(cbHolder->Release());
			}

			MS_ERROR_NO_THROW("cannot prepare TCP io_uring write: unknown exception");
			HandleWriteFailure(cbOwner.release());

			return;
		}
	}

write_libuv:
#endif

	// First try uv_try_write(). In case it can not directly write all the given
	// data then build a uv_req_t and use uv_write().

	const size_t totalLen = len1 + len2;
	uv_buf_t buffers[2];
	int written{ 0 };
	int err;
#ifdef MS_TEST
	const bool forceWriteDataAllocationFailure = failNextWriteDataAllocationForTesting;
	failNextWriteDataAllocationForTesting       = false;
#endif

	buffers[0] = uv_buf_init(reinterpret_cast<char*>(const_cast<uint8_t*>(data1)), len1);
	buffers[1] = uv_buf_init(reinterpret_cast<char*>(const_cast<uint8_t*>(data2)), len2);
#ifdef MS_TEST
	if (forceWriteDataAllocationFailure)
	{
		// Make the allocation path deterministic without putting a partial frame on
		// the wire. The injected failure is raised at the real ownership hand-off
		// point below.
		written = UV_EAGAIN;
	}
	else
#endif
	{
		written = uv_try_write(reinterpret_cast<uv_stream_t*>(this->uvHandle), buffers, 2);
	}

	// All the data was written. Done.
	if (written == static_cast<int>(totalLen))
	{
		// Update sent bytes.
		this->sentBytes += written;

		auto* closeListener = InvokeSendCallbackNoThrow(cbOwner.get(), true);
		cbOwner.reset();
		notifyConnectionClosedNoThrow(this, closeListener);

		return;
	}
	// Cannot write any data at first time. Use uv_write().
	else if (written == UV_EAGAIN || written == UV_ENOSYS)
	{
		// Set written to 0 so pendingLen can be properly calculated.
		written = 0;
	}
	// Any other error.
	else if (written < 0)
	{
		MS_WARN_DEV_NO_THROW("uv_try_write() failed, trying uv_write(): %s", uv_strerror(written));

		// Set written to 0 so pendingLen can be properly calculated.
		written = 0;
	}

	const size_t pendingLen = totalLen - written;
	std::unique_ptr<UvWriteData> writeData;

	try
	{
#ifdef MS_TEST
		if (forceWriteDataAllocationFailure)
		{
			throw std::bad_alloc();
		}
#endif

		writeData = std::make_unique<UvWriteData>(pendingLen);
	}
	catch (const std::exception& error)
	{
		MS_ERROR_NO_THROW("cannot allocate TCP pending-write data: %s", error.what());
		HandleWriteFailure(cbOwner.release());

		return;
	}
	catch (...)
	{
		MS_ERROR_NO_THROW("cannot allocate TCP pending-write data: unknown exception");
		HandleWriteFailure(cbOwner.release());

		return;
	}

	writeData->req.data = static_cast<void*>(writeData.get());

	// If the first buffer was not entirely written then splice it.
	if (static_cast<size_t>(written) < len1)
	{
		const size_t pendingLen1 = len1 - static_cast<size_t>(written);

		std::memcpy(writeData->store, data1 + static_cast<size_t>(written), pendingLen1);

		// A null buffer is valid when its length is zero, but passing it to
		// memcpy() is still undefined behavior even with a zero byte count.
		if (len2 != 0u)
		{
			std::memcpy(writeData->store + pendingLen1, data2, len2);
		}
	}
	// Otherwise just take the pending data in the second buffer.
	else
	{
		const size_t data2Offset = static_cast<size_t>(written) - len1;
		const size_t pendingLen2 = len2 - data2Offset;

		if (pendingLen2 != 0u)
		{
			std::memcpy(writeData->store, data2 + data2Offset, pendingLen2);
		}
	}

	writeData->cb = cbOwner.release();

	const uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(writeData->store), pendingLen);

	err = uv_write(
	  &writeData->req,
	  reinterpret_cast<uv_stream_t*>(this->uvHandle),
	  &buffer,
	  1,
	  static_cast<uv_write_cb>(onWrite));

	if (err != 0)
	{
		MS_WARN_DEV_NO_THROW("uv_write() failed: %s", uv_strerror(err));
		cbOwner.reset(writeData->cb);
		writeData->cb = nullptr;
		writeData.reset();
		HandleWriteFailure(cbOwner.release());
	}
	else
	{
		writeData.release();

		// Update sent bytes.
		this->sentBytes += pendingLen;
	}
}

void TcpConnectionHandle::ErrorReceiving() noexcept
{
	MS_TRACE_NO_THROW();

	if (this->closed)
	{
		return;
	}

	this->hasError = true;

	InternalClose();

	NotifyClosed();
}

void TcpConnectionHandle::BeginUvCallback() noexcept
{
	++this->uvCallbackDepth;
}

TcpConnectionHandle::Listener* TcpConnectionHandle::EndUvCallback() noexcept
{
	if (this->uvCallbackDepth == 0u)
	{
		MS_ERROR_NO_THROW("unbalanced TCP libuv callback lifetime boundary");

		return nullptr;
	}

	--this->uvCallbackDepth;

	if (
	  this->uvCallbackDepth != 0u || !this->closeNotificationPending ||
	  this->closeNotificationSent)
	{
		return nullptr;
	}

	this->closeNotificationPending = false;
	this->closeNotificationSent    = true;

	return this->listener;
}

void TcpConnectionHandle::InternalClose() noexcept
{
	MS_TRACE_NO_THROW();

	if (this->closed)
	{
		return;
	}

	int err;

	this->closed = true;

	if (!this->uvHandle)
	{
		return;
	}

	// Setup may fail before uv_tcp_init() publishes the raw handle to libuv. In
	// that state it must be deleted directly rather than passed to uv_close().
	if (!this->uvHandleInitialized)
	{
		delete this->uvHandle;
		this->uvHandle = nullptr;

		return;
	}

	// Tell the UV handle that the TcpConnectionHandle has been closed.
	this->uvHandle->data = nullptr;

	// Don't read more.
	err = uv_read_stop(reinterpret_cast<uv_stream_t*>(this->uvHandle));

	if (err != 0)
	{
		MS_ERROR_NO_THROW("uv_read_stop() failed while closing TCP connection: %s", uv_strerror(err));
	}

	// If there is no error and the peer didn't close its connection side then close gracefully.
	if (!this->hasError && !this->isClosedByPeer)
	{
		// Use uv_shutdown() so pending data to be written will be sent to the peer
		// before closing.
		uv_shutdown_t* req{ nullptr };

#ifdef MS_TEST
		if (failNextShutdownAllocationForTesting)
		{
			failNextShutdownAllocationForTesting = false;
		}
		else
#endif
		{
			req = new (std::nothrow) uv_shutdown_t;
		}

		if (!req)
		{
			MS_ERROR_NO_THROW("cannot allocate uv_shutdown_t, closing TCP connection immediately");
			uv_close(
			  reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseTcp));

			return;
		}

		req->data = static_cast<void*>(this);
		err       = uv_shutdown(
		  req, reinterpret_cast<uv_stream_t*>(this->uvHandle), static_cast<uv_shutdown_cb>(onShutdown));

		if (err != 0)
		{
			MS_ERROR_NO_THROW(
			  "uv_shutdown() failed, closing TCP connection immediately: %s", uv_strerror(err));
			delete req;
			uv_close(
			  reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseTcp));
		}
	}
	// Otherwise directly close the socket.
	else
	{
		uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseTcp));
	}
}

void TcpConnectionHandle::NotifyClosed() noexcept
{
	if (this->closeNotificationSent)
	{
		return;
	}

	if (this->uvCallbackDepth != 0u)
	{
		this->closeNotificationPending = true;

		return;
	}

	this->closeNotificationSent = true;
	notifyConnectionClosedNoThrow(this, this->listener);
}

void TcpConnectionHandle::HandleWriteFailure(onSendCallback* cb) noexcept
{
	std::unique_ptr<onSendCallback> cbOwner(cb);

	if (!this->closed)
	{
		this->hasError = true;
		InternalClose();
	}

	auto* closeListener = InvokeSendCallbackNoThrow(cbOwner.get(), false);

	// A callback may capture connection-owned state. Release it before the close
	// notification, which is allowed to delete this connection synchronously.
	cbOwner.reset();

	if (closeListener)
	{
		notifyConnectionClosedNoThrow(this, closeListener);
	}
	else
	{
		NotifyClosed();
	}
}

TcpConnectionHandle::Listener* TcpConnectionHandle::InvokeSendCallbackNoThrow(
  onSendCallback* cb, bool sent) noexcept
{
	if (!cb)
	{
		return nullptr;
	}

	BeginUvCallback();

	try
	{
		(*cb)(sent);
	}
	catch (const std::exception& error)
	{
		MS_ERROR_NO_THROW("TCP send callback failed: %s", error.what());
	}
	catch (...)
	{
		MS_ERROR_NO_THROW("TCP send callback failed with an unknown exception");
	}

	return EndUvCallback();
}

#ifdef MS_TEST
void TcpConnectionHandle::FailNextSetupAfterUvInitForTesting()
{
	failNextSetupAfterUvInitForTesting = true;
}

void TcpConnectionHandle::FailNextStartAfterReadStartForTesting()
{
	failNextStartAfterReadStartForTesting = true;
}

void TcpConnectionHandle::FailNextWriteDataAllocationForTesting()
{
	failNextWriteDataAllocationForTesting = true;
}

void TcpConnectionHandle::FailNextShutdownAllocationForTesting()
{
	failNextShutdownAllocationForTesting = true;
}

size_t TcpConnectionHandle::GetTcpCloseCountForTesting()
{
	return tcpCloseCountForTesting;
}
#endif

bool TcpConnectionHandle::SetPeerAddress()
{
	MS_TRACE();

	int err;
	int len = sizeof(this->peerAddr);

	err = uv_tcp_getpeername(this->uvHandle, reinterpret_cast<struct sockaddr*>(&this->peerAddr), &len);

	if (err != 0)
	{
		MS_ERROR("uv_tcp_getpeername() failed: %s", uv_strerror(err));

		return false;
	}

	int family;

	Utils::IP::GetAddressInfo(
	  reinterpret_cast<const struct sockaddr*>(&this->peerAddr), family, this->peerIp, this->peerPort);

	return true;
}

inline void TcpConnectionHandle::OnUvReadAlloc(size_t /*suggestedSize*/, uv_buf_t* buf) noexcept
{
	MS_TRACE_NO_THROW();

	// If this is the first call to onUvReadAlloc() then allocate the receiving buffer now.
	if (!this->buffer)
	{
		this->buffer = new (std::nothrow) uint8_t[this->bufferSize];

		if (!this->buffer)
		{
			MS_ERROR_NO_THROW("cannot allocate TCP receive buffer");
			buf->base = nullptr;
			buf->len  = 0u;

			return;
		}
	}

	// Tell UV to write after the last data byte in the buffer.
	buf->base = reinterpret_cast<char*>(this->buffer + this->bufferDataLen);

	// Give UV all the remaining space in the buffer.
	if (this->bufferSize > this->bufferDataLen)
	{
		buf->len = this->bufferSize - this->bufferDataLen;
	}
	else
	{
		buf->len = 0;

		MS_WARN_DEV_NO_THROW("no available space in the buffer");
	}
}

inline void TcpConnectionHandle::OnUvRead(ssize_t nread, const uv_buf_t* /*buf*/) noexcept
{
	MS_TRACE_NO_THROW();

	if (nread == 0)
	{
		return;
	}

	// Data received.
	if (nread > 0)
	{
		// Update received bytes.
		this->recvBytes += nread;

		// Update the buffer data length.
		this->bufferDataLen += static_cast<size_t>(nread);

		// Notify the subclass. Packet parsing and listener code may allocate, but
		// this method is a libuv callback boundary and must fail closed instead of
		// unwinding through C.
		try
		{
			UserOnTcpConnectionRead();
		}
		catch (const std::exception& error)
		{
			MS_ERROR_NO_THROW("TCP read listener failed: %s", error.what());
			ErrorReceiving();
		}
		catch (...)
		{
			MS_ERROR_NO_THROW("TCP read listener failed with an unknown exception");
			ErrorReceiving();
		}
	}
	// Client disconnected.
	else if (nread == UV_EOF || nread == UV_ECONNRESET)
	{
		MS_DEBUG_DEV_NO_THROW("connection closed by peer, closing server side");

		this->isClosedByPeer = true;

		// Close server side of the connection.
		InternalClose();

		// Notify the listener.
		NotifyClosed();
	}
	// Some error.
	else
	{
		MS_WARN_DEV_NO_THROW("read error, closing the connection: %s", uv_strerror(nread));

		this->hasError = true;

		// Close server side of the connection.
		InternalClose();

		// Notify the listener.
		NotifyClosed();
	}
}

inline void TcpConnectionHandle::OnUvWrite(
  int status, TcpConnectionHandle::onSendCallback* cb) noexcept
{
	MS_TRACE_NO_THROW();

	// NOTE: Do not delete cb here since it will be delete in onWrite() above.

	if (status == 0)
	{
		if (cb)
		{
			try
			{
				(*cb)(true);
			}
			catch (const std::exception& error)
			{
				MS_ERROR_NO_THROW("TCP write completion callback failed: %s", error.what());
			}
			catch (...)
			{
				MS_ERROR_NO_THROW("TCP write completion callback failed with an unknown exception");
			}
		}
	}
	else
	{
		if (status != UV_EPIPE && status != UV_ENOTCONN)
		{
			this->hasError = true;
		}

		MS_WARN_DEV_NO_THROW("write error, closing the connection: %s", uv_strerror(status));

		// Commit the closed state before invoking user code. A callback may
		// re-enter TriggerClose(); seeing closed=true prevents it from publishing
		// and deleting this connection before this completion handler unwinds.
		InternalClose();

		if (cb)
		{
			try
			{
				(*cb)(false);
			}
			catch (const std::exception& error)
			{
				MS_ERROR_NO_THROW("TCP write failure callback failed: %s", error.what());
			}
			catch (...)
			{
				MS_ERROR_NO_THROW("TCP write failure callback failed with an unknown exception");
			}
		}

		NotifyClosed();
	}
}

#ifdef MS_LIBURING_SUPPORTED
inline void TcpConnectionHandle::OnLibUringWrite(
  bool sent, size_t len, TcpConnectionHandle::onSendCallback* cb) noexcept
{
	MS_TRACE_NO_THROW();

	std::unique_ptr<onSendCallback> cbOwner(cb);

	if (sent)
	{
		this->sentBytes += len;
		auto* closeListener = InvokeSendCallbackNoThrow(cbOwner.get(), true);
		cbOwner.reset();
		notifyConnectionClosedNoThrow(this, closeListener);

		return;
	}

	if (!this->closed)
	{
		this->hasError = true;

		MS_WARN_DEV_NO_THROW("incomplete io_uring write, closing the connection");

		InternalClose();
	}

	auto* closeListener = InvokeSendCallbackNoThrow(cbOwner.get(), false);
	cbOwner.reset();

	if (closeListener)
	{
		notifyConnectionClosedNoThrow(this, closeListener);
	}
	else
	{
		NotifyClosed();
	}
}
#endif

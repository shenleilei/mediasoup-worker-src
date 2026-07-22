#define MS_CLASS "UdpSocketHandle"
// #define MS_LOG_DEV_LEVEL 3

#include "handles/UdpSocketHandle.hpp"
#ifdef MS_LIBURING_SUPPORTED
#include "DepLibUring.hpp"
#endif
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include <cerrno>
#include <cstdlib> // std::getenv()
#include <cstring> // std::memcpy()
#include <exception>
#ifdef __linux__
#include <algorithm>
#include <sys/socket.h>
#endif

/* Static. */

// libuv splits a UV_UDP_RECVMMSG allocation into 64KiB datagram slots. A
// single-slot allocation keeps the recvmmsg path functionally correct but still
// performs one syscall per packet, which is especially costly for high
// packet-rate RTP workloads.
static constexpr size_t UdpDatagramBufferSize{ 65536 };
// Eight slots reduce receive syscalls by up to 8x while preserving libuv's
// existing per-I/O-turn budget of 32 datagrams without overshooting it.
static constexpr size_t UdpReceiveBatchSize{ 8 };
static constexpr size_t ReadBufferSize{ UdpDatagramBufferSize * UdpReceiveBatchSize };
static_assert(ReadBufferSize / UdpDatagramBufferSize == UdpReceiveBatchSize);

#ifdef __linux__
static constexpr size_t MaxSendmmsgBatchSize{ 64u };

static size_t GetConfiguredSendmmsgBatchSize()
{
	const char* value = std::getenv("MEDIASOUP_WORKER_UDP_SENDMMSG_BATCH_SIZE");

	if (!value || value[0] == '\0')
	{
		return 0u;
	}

	char* end{ nullptr };
	errno = 0;
	const auto parsed = std::strtoul(value, std::addressof(end), 10);

	if (errno != 0 || end == value || *end != '\0')
	{
		return 0u;
	}

	if (parsed <= 1u)
	{
		return 0u;
	}

	return std::min<size_t>(static_cast<size_t>(parsed), MaxSendmmsgBatchSize);
}

static bool GetSockaddrLen(const struct sockaddr* addr, socklen_t& len)
{
	if (!addr)
	{
		return false;
	}

	switch (addr->sa_family)
	{
		case AF_INET:
		{
			len = sizeof(struct sockaddr_in);

			return true;
		}

		case AF_INET6:
		{
			len = sizeof(struct sockaddr_in6);

			return true;
		}

		default:
		{
			return false;
		}
	}
}

static bool IsLoopbackAddress(const struct sockaddr* addr)
{
	if (!addr)
	{
		return false;
	}

	switch (addr->sa_family)
	{
		case AF_INET:
		{
			const auto* in = reinterpret_cast<const struct sockaddr_in*>(addr);

			return (ntohl(in->sin_addr.s_addr) >> 24u) == 127u;
		}

		case AF_INET6:
		{
			const auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(addr);

			return IN6_IS_ADDR_LOOPBACK(std::addressof(in6->sin6_addr));
		}

		default:
		{
			return false;
		}
	}
}
#endif

// RTP/RTCP/STUN parsing reads multi-byte fields from this storage, so preserve
// explicit alignment while increasing the thread-local batch allocation.
alignas(4) thread_local static uint8_t ReadBuffer[ReadBufferSize];

#ifdef MS_TEST
thread_local static bool failNextFilenoForTesting{ false };
#endif

/* Static methods for UV callbacks. */

inline static void onAlloc(uv_handle_t* handle, size_t suggestedSize, uv_buf_t* buf)
{
	auto* socket = static_cast<UdpSocketHandle*>(handle->data);

	if (socket)
	{
		socket->OnUvRecvAlloc(suggestedSize, buf);
	}
}

inline static void onRecv(
  uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags)
{
	auto* socket = static_cast<UdpSocketHandle*>(handle->data);

	if (socket)
	{
		socket->OnUvRecv(nread, buf, addr, flags);
	}
}

inline static void onSend(uv_udp_send_t* req, int status)
{
	auto* sendData = static_cast<UdpSocketHandle::UvSendData*>(req->data);
	auto* handle   = req->handle;
	auto* socket   = static_cast<UdpSocketHandle*>(handle->data);
	const auto* cb = sendData->cb;

	if (socket)
	{
		socket->OnUvSend(status, cb);
	}

	// Delete the UvSendData struct (it will delete the store and cb too).
	delete sendData;
}

#ifdef __linux__
inline static void onBatchedSendCheck(uv_check_t* handle)
{
	auto* socket = static_cast<UdpSocketHandle*>(handle->data);

	if (socket)
	{
		socket->OnUvBatchedSendCheck();
	}
}

inline static void onCloseBatchedSendCheck(uv_handle_t* handle)
{
	delete reinterpret_cast<uv_check_t*>(handle);
}
#endif

inline static void onCloseUdp(uv_handle_t* handle)
{
	delete reinterpret_cast<uv_udp_t*>(handle);
}

/* Instance methods. */

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
UdpSocketHandle::UdpSocketHandle(uv_udp_t* uvHandle) : uvHandle(uvHandle)
{
	MS_TRACE();

	this->uvHandle->data = static_cast<void*>(this);

	// NOLINTNEXTLINE(misc-const-correctness)
	int err = uv_udp_recv_start(
	  this->uvHandle, static_cast<uv_alloc_cb>(onAlloc), static_cast<uv_udp_recv_cb>(onRecv));

	if (err != 0)
	{
		this->uvHandle->data = nullptr;
		uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseUdp));

		MS_THROW_ERROR("uv_udp_recv_start() failed: %s", uv_strerror(err));
	}

	// Set local address.
	if (!SetLocalAddress())
	{
		this->uvHandle->data = nullptr;
		uv_udp_recv_stop(this->uvHandle);
		uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseUdp));

		MS_THROW_ERROR("error setting local IP and port");
	}

#if defined(MS_LIBURING_SUPPORTED) || defined(__linux__)
#ifdef MS_TEST
	if (failNextFilenoForTesting)
	{
		failNextFilenoForTesting = false;
		err                         = UV_EBADF;
	}
	else
#endif
	{
		err = uv_fileno(reinterpret_cast<uv_handle_t*>(this->uvHandle), std::addressof(this->fd));
	}

	if (err != 0)
	{
#ifdef MS_LIBURING_SUPPORTED
		this->uvHandle->data = nullptr;
		uv_udp_recv_stop(this->uvHandle);
		uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseUdp));

		MS_THROW_ERROR("uv_fileno() failed: %s", uv_strerror(err));
#else
		this->fd = -1;
#endif
	}
#endif

#ifdef __linux__
	this->sendmmsgBatchSize = GetConfiguredSendmmsgBatchSize();

	if (this->sendmmsgBatchSize > 1u && this->fd >= 0)
	{
		this->batchedSendCheckHandle       = new uv_check_t;
		this->batchedSendCheckHandle->data = static_cast<void*>(this);

		err = uv_check_init(this->uvHandle->loop, this->batchedSendCheckHandle);

		if (err != 0)
		{
			delete this->batchedSendCheckHandle;
			this->batchedSendCheckHandle = nullptr;
			this->sendmmsgBatchSize      = 0u;

			MS_WARN_DEV("uv_check_init() failed, disabling UDP sendmmsg batching: %s", uv_strerror(err));
		}
		else
		{
			MS_DEBUG_DEV("UDP sendmmsg batching enabled [batchSize:%zu]", this->sendmmsgBatchSize);
		}
	}
#endif
}

UdpSocketHandle::~UdpSocketHandle()
{
	MS_TRACE();

	if (!this->closed)
	{
		InternalClose();
	}
}

void UdpSocketHandle::Dump() const
{
	MS_DUMP("<UdpSocketHandle>");
	MS_DUMP("  localIp: %s", this->localIp.c_str());
	MS_DUMP("  localPort: %" PRIu16, static_cast<uint16_t>(this->localPort));
	MS_DUMP("  closed: %s", this->closed ? "yes" : "no");
	MS_DUMP("</UdpSocketHandle>");
}

void UdpSocketHandle::Send(
  const uint8_t* data, size_t len, const struct sockaddr* addr, UdpSocketHandle::onSendCallback* cb)
{
	MS_TRACE();

	if (this->closed || !data || len == 0u || !addr)
	{
		if (cb)
		{
			(*cb)(false);
			delete cb;
		}

		return;
	}

#ifdef __linux__
	if (EnqueueBatchedSend(data, len, addr, cb))
	{
		return;
	}
#endif

	SendImmediate(data, len, addr, cb);
}

void UdpSocketHandle::SendImmediate(
  const uint8_t* data, size_t len, const struct sockaddr* addr, UdpSocketHandle::onSendCallback* cb)
{
	MS_TRACE();

#ifdef MS_LIBURING_SUPPORTED
	{
		if (!DepLibUring::IsActive())
		{
			goto send_libuv;
		}

		// Prepare the data to be sent.
		// NOTE: If all SQEs are currently in use or no UserData entry is available we'll
		// fall back to libuv.
		auto prepared = DepLibUring::PrepareSend(this->fd, data, len, addr, cb);

		if (!prepared)
		{
			MS_DEBUG_DEV("cannot send via liburing, fallback to libuv");

			goto send_libuv;
		}

		return;
	}

send_libuv:
#endif

	// First try uv_udp_try_send(). In case it can not directly send the datagram
	// then build a uv_req_t and use uv_udp_send().

	uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(const_cast<uint8_t*>(data)), len);
	const int sent  = uv_udp_try_send(this->uvHandle, &buffer, 1, addr);

	// Entire datagram was sent. Done.
	if (sent == static_cast<int>(len))
	{
		// Update sent bytes.
		this->sentBytes += sent;

		if (cb)
		{
			(*cb)(true);
			delete cb;
		}

		return;
	}
	else if (sent >= 0)
	{
		MS_WARN_DEV("datagram truncated (just %d of %zu bytes were sent)", sent, len);

		// Update sent bytes.
		this->sentBytes += sent;

		if (cb)
		{
			(*cb)(false);
			delete cb;
		}

		return;
	}
	// Any error but legit EAGAIN. Use uv_udp_send().
	else if (sent != UV_EAGAIN)
	{
		MS_WARN_DEV("uv_udp_try_send() failed, trying uv_udp_send(): %s", uv_strerror(sent));
	}

	auto* sendData = new UvSendData(len);

	sendData->req.data = static_cast<void*>(sendData);
	std::memcpy(sendData->store, data, len);
	sendData->cb = cb;

	buffer = uv_buf_init(reinterpret_cast<char*>(sendData->store), len);

	const int err = uv_udp_send(
	  &sendData->req, this->uvHandle, &buffer, 1, addr, static_cast<uv_udp_send_cb>(onSend));

	if (err != 0)
	{
		// NOTE: uv_udp_send() returns error if a wrong INET family is given
		// (IPv6 destination on a IPv4 binded socket), so be ready.
		MS_WARN_DEV("uv_udp_send() failed: %s", uv_strerror(err));

		if (cb)
		{
			(*cb)(false);
		}

		// Delete the UvSendData struct (it will delete the store and cb too).
		delete sendData;
	}
	else
	{
		// Update sent bytes.
		this->sentBytes += len;
	}
}

#ifdef __linux__
bool UdpSocketHandle::EnqueueBatchedSend(
  const uint8_t* data, size_t len, const struct sockaddr* addr, UdpSocketHandle::onSendCallback* cb)
{
	MS_TRACE();

	if (
	  this->sendmmsgBatchSize <= 1u || this->fd < 0 || !this->batchedSendCheckHandle ||
	  this->flushingBatchedSends)
	{
		return false;
	}

#ifdef MS_LIBURING_SUPPORTED
	if (DepLibUring::IsActive())
	{
		return false;
	}
#endif

	socklen_t addrLen{ 0u };

	if (!GetSockaddrLen(addr, addrLen))
	{
		return false;
	}

	if (!IsLoopbackAddress(addr))
	{
		return false;
	}

	PendingSendData pending;
	pending.addr    = Utils::IP::CopyAddress(addr);
	pending.addrLen = addrLen;
	pending.len     = len;
	pending.cb      = cb;

	if (len <= PendingSendData::InlineCapacity)
	{
		std::memcpy(pending.inlineStore.data(), data, len);
	}
	else
	{
		pending.heapStore.reset(new uint8_t[len]);
		std::memcpy(pending.heapStore.get(), data, len);
	}

	this->pendingBatchedSends.emplace_back(std::move(pending));

	if (this->pendingBatchedSends.size() >= this->sendmmsgBatchSize)
	{
		FlushBatchedSends();
	}
	else
	{
		StartBatchedSendCheck();
	}

	return true;
}

void UdpSocketHandle::StartBatchedSendCheck()
{
	MS_TRACE();

	if (!this->batchedSendCheckHandle || this->batchedSendCheckActive)
	{
		return;
	}

	const int err = uv_check_start(
	  this->batchedSendCheckHandle, static_cast<uv_check_cb>(onBatchedSendCheck));

	if (err != 0)
	{
		MS_WARN_DEV("uv_check_start() failed, flushing UDP sendmmsg batch immediately: %s", uv_strerror(err));
		FlushBatchedSends();

		return;
	}

	this->batchedSendCheckActive = true;
}

void UdpSocketHandle::StopBatchedSendCheck()
{
	MS_TRACE();

	if (!this->batchedSendCheckHandle || !this->batchedSendCheckActive)
	{
		return;
	}

	uv_check_stop(this->batchedSendCheckHandle);
	this->batchedSendCheckActive = false;
}

void UdpSocketHandle::FlushBatchedSends()
{
	MS_TRACE();

	if (this->pendingBatchedSends.empty() || this->flushingBatchedSends)
	{
		return;
	}

	StopBatchedSendCheck();

	this->flushingBatchedSends = true;

	while (!this->pendingBatchedSends.empty())
	{
		const auto batchCount =
		  std::min<size_t>(this->pendingBatchedSends.size(), this->sendmmsgBatchSize);
		std::array<mmsghdr, MaxSendmmsgBatchSize> messages{};
		std::array<iovec, MaxSendmmsgBatchSize> iovecs{};

		for (size_t idx{ 0u }; idx < batchCount; ++idx)
		{
			auto& pending = this->pendingBatchedSends[idx];

			iovecs[idx].iov_base              = pending.Data();
			iovecs[idx].iov_len               = pending.len;
			messages[idx].msg_hdr.msg_iov     = std::addressof(iovecs[idx]);
			messages[idx].msg_hdr.msg_iovlen  = 1;
			messages[idx].msg_hdr.msg_name    = std::addressof(pending.addr);
			messages[idx].msg_hdr.msg_namelen = pending.addrLen;
		}

		int sent{ 0 };

		while (true)
		{
			sent = ::sendmmsg(this->fd, messages.data(), static_cast<unsigned int>(batchCount), 0);

			if (sent < 0 && errno == EINTR)
			{
				continue;
			}

			break;
		}

		if (sent > 0)
		{
			for (int idx{ 0 }; idx < sent; ++idx)
			{
				auto& pending = this->pendingBatchedSends.front();
				const auto msgLen = static_cast<size_t>(messages[static_cast<size_t>(idx)].msg_len);
				const bool ok     = msgLen == pending.len;

				this->sentBytes += msgLen;

				if (pending.cb)
				{
					(*pending.cb)(ok);
					delete pending.cb;
					pending.cb = nullptr;
				}

				this->pendingBatchedSends.pop_front();
			}

			if (static_cast<size_t>(sent) < batchCount)
			{
				FlushBatchedSendsWithImmediateFallback();
				break;
			}

			continue;
		}

		FlushBatchedSendsWithImmediateFallback();
		break;
	}

	this->flushingBatchedSends = false;
}

void UdpSocketHandle::FlushBatchedSendsWithImmediateFallback()
{
	MS_TRACE();

	std::deque<PendingSendData> pending;
	pending.swap(this->pendingBatchedSends);

	for (auto& item : pending)
	{
		auto* cb = item.cb;
		item.cb  = nullptr;
		SendImmediate(
		  item.Data(),
		  item.len,
		  reinterpret_cast<const struct sockaddr*>(std::addressof(item.addr)),
		  cb);
	}
}

void UdpSocketHandle::FailPendingBatchedSends()
{
	MS_TRACE();

	StopBatchedSendCheck();

	while (!this->pendingBatchedSends.empty())
	{
		auto& pending = this->pendingBatchedSends.front();

		if (pending.cb)
		{
			try
			{
				(*pending.cb)(false);
			}
			catch (const std::exception& error)
			{
				MS_ERROR("UDP batched send close callback failed: %s", error.what());
			}
			catch (...)
			{
				MS_ERROR("UDP batched send close callback failed");
			}

			delete pending.cb;
			pending.cb = nullptr;
		}

		this->pendingBatchedSends.pop_front();
	}
}

void UdpSocketHandle::OnUvBatchedSendCheck()
{
	MS_TRACE();

	FlushBatchedSends();
}
#endif

uint32_t UdpSocketHandle::GetSendBufferSize() const
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

void UdpSocketHandle::SetSendBufferSize(uint32_t size)
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

uint32_t UdpSocketHandle::GetRecvBufferSize() const
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

void UdpSocketHandle::SetRecvBufferSize(uint32_t size)
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

void UdpSocketHandle::InternalClose() noexcept
{
	MS_TRACE();

	if (this->closed)
	{
		return;
	}

	this->closed = true;

#ifdef __linux__
	FailPendingBatchedSends();

	if (this->batchedSendCheckHandle)
	{
		this->batchedSendCheckHandle->data = nullptr;

		if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(this->batchedSendCheckHandle)))
		{
			uv_close(
			  reinterpret_cast<uv_handle_t*>(this->batchedSendCheckHandle),
			  static_cast<uv_close_cb>(onCloseBatchedSendCheck));
		}

		this->batchedSendCheckHandle = nullptr;
	}
#endif

	// Tell the UV handle that the UdpSocketHandle has been closed.
	this->uvHandle->data = nullptr;

	// Don't read more.
	const int err = uv_udp_recv_stop(this->uvHandle);

	if (err != 0)
	{
		MS_ERROR("uv_udp_recv_stop() failed while closing UDP socket: %s", uv_strerror(err));
	}

	uv_close(reinterpret_cast<uv_handle_t*>(this->uvHandle), static_cast<uv_close_cb>(onCloseUdp));
}

#ifdef MS_TEST
void UdpSocketHandle::FailNextFilenoForTesting()
{
	failNextFilenoForTesting = true;
}

size_t UdpSocketHandle::GetReadBufferSizeForTesting()
{
	return ReadBufferSize;
}
#endif

bool UdpSocketHandle::SetLocalAddress()
{
	MS_TRACE();

	int err;
	int len = sizeof(this->localAddr);

	err =
	  uv_udp_getsockname(this->uvHandle, reinterpret_cast<struct sockaddr*>(&this->localAddr), &len);

	if (err != 0)
	{
		MS_ERROR("uv_udp_getsockname() failed: %s", uv_strerror(err));

		return false;
	}

	int family;

	Utils::IP::GetAddressInfo(
	  reinterpret_cast<const struct sockaddr*>(&this->localAddr), family, this->localIp, this->localPort);

	return true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
inline void UdpSocketHandle::OnUvRecvAlloc(size_t /*suggestedSize*/, uv_buf_t* buf)
{
	MS_TRACE();

	// Tell UV to write into the static buffer.
	buf->base = reinterpret_cast<char*>(ReadBuffer);
	// Give UV all the buffer space.
	buf->len = ReadBufferSize;
}

inline void UdpSocketHandle::OnUvRecv(
  ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags)
{
	MS_TRACE();

	// NOTE: Ignore if there is nothing to read or if it was an empty datagram.
	if (nread == 0)
	{
		return;
	}

	// Check flags.
	if ((flags & UV_UDP_PARTIAL) != 0u)
	{
		MS_ERROR("received datagram was truncated due to insufficient buffer, ignoring it");

		return;
	}

	// Data received.
	if (nread > 0)
	{
		// Update received bytes.
		this->recvBytes += nread;

		// Notify the subclass.
		UserOnUdpDatagramReceived(reinterpret_cast<uint8_t*>(buf->base), nread, addr);
	}
	// Some error.
	else
	{
		MS_DEBUG_DEV("read error: %s", uv_strerror(nread));
	}
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
inline void UdpSocketHandle::OnUvSend(int status, UdpSocketHandle::onSendCallback* cb)
{
	MS_TRACE();

	// NOTE: Do not delete cb here since it will be delete in onSend() above.

	if (status == 0)
	{
		if (cb)
		{
			(*cb)(true);
		}
	}
	else
	{
#if MS_LOG_DEV_LEVEL == 3
		MS_DEBUG_DEV("send error: %s", uv_strerror(status));
#endif

		if (cb)
		{
			(*cb)(false);
		}
	}
}

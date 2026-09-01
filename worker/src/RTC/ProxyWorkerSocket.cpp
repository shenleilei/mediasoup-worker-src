#define MS_CLASS "RTC::ProxyWorkerSocket"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/ProxyWorkerSocket.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "RTC/ProxyWorkerIpc.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace RTC
{
	namespace
	{
		constexpr size_t ReceiveBatchSize{ 16u };
		constexpr size_t MaxSendDatagramsPerPoll{ 128u };
		constexpr auto MaxSendIoWorkPerPoll = std::chrono::milliseconds(1);

		bool IsTransientSendError(int error)
		{
			return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS;
		}

		bool ShouldLogDropCounter(uint64_t count)
		{
			return count <= 4u || (count & (count - 1u)) == 0u;
		}

		void OnUvPollEvent(uv_poll_t* handle, int status, int events)
		{
			auto* socket = static_cast<RTC::ProxyWorkerSocket*>(handle->data);

			if (socket)
			{
				socket->OnUvPoll(status, events);
			}
		}

		void OnUvClose(uv_handle_t* handle)
		{
			delete reinterpret_cast<uv_poll_t*>(handle);
		}

		int SetNonBlocking(int fd)
		{
			const int flags = ::fcntl(fd, F_GETFL, 0);

			if (flags < 0)
			{
				return -1;
			}

			return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}

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
	} // namespace

	struct ProxyWorkerSocket::ReceiveBatchStorage
	{
		std::array<std::array<uint8_t, RTC::ProxyWorkerIpc::kMaxFrameSize>, ReceiveBatchSize> buffers{};
		std::array<sockaddr_un, ReceiveBatchSize> peerAddrs{};
		std::array<iovec, ReceiveBatchSize> iovecs{};
		std::array<mmsghdr, ReceiveBatchSize> messages{};

		void Prepare(size_t count)
		{
			for (size_t idx{ 0u }; idx < count; ++idx)
			{
				this->peerAddrs[idx]                    = {};
				this->iovecs[idx]                       = {};
				this->messages[idx]                     = {};
				this->iovecs[idx].iov_base              = this->buffers[idx].data();
				this->iovecs[idx].iov_len               = this->buffers[idx].size();
				this->messages[idx].msg_hdr.msg_name    = std::addressof(this->peerAddrs[idx]);
				this->messages[idx].msg_hdr.msg_namelen = sizeof(sockaddr_un);
				this->messages[idx].msg_hdr.msg_iov     = std::addressof(this->iovecs[idx]);
				this->messages[idx].msg_hdr.msg_iovlen  = 1u;
			}
		}
	};

	ProxyWorkerSocket::ProxyWorkerSocket(
	  Listener* listener, std::string path, const struct sockaddr* localAddress)
	  : listener(listener), path(std::move(path))
	{
		MS_TRACE();

		if (!this->listener)
		{
			MS_THROW_TYPE_ERROR("proxy-worker socket listener is missing");
		}
		if (!localAddress)
		{
			MS_THROW_TYPE_ERROR("proxy-worker socket local address is missing");
		}
		this->receiveBatchStorage = std::make_unique<ReceiveBatchStorage>();
		this->sendExpiryTimer     = std::make_unique<TimerHandle>(this);

		if (const char* raw = std::getenv("MEDIASOUP_WORKER_PROXY_UDS_MAX_RECV_PER_POLL"))
		{
			const auto value = std::strtoull(raw, nullptr, 10);
			if (value >= 1u && value <= 65536u)
			{
				this->maxReceiveDatagramsPerPoll = static_cast<size_t>(value);
			}
		}
		if (const char* raw = std::getenv("MEDIASOUP_WORKER_PROXY_UDS_MAX_RECV_MS"))
		{
			const auto value = std::strtoull(raw, nullptr, 10);
			if (value >= 1u && value <= 1000u)
			{
				this->maxReceiveWorkPerPollMs = value;
			}
		}

		size_t localAddressLen{ 0u };
		if (!RTC::ProxyWorkerIpc::SockaddrLength(localAddress, localAddressLen))
		{
			MS_THROW_TYPE_ERROR("proxy-worker socket local address must be IPv4 or IPv6");
		}
		std::memcpy(&this->localAddr, localAddress, localAddressLen);

		sockaddr_un bindAddr{};
		socklen_t bindAddrLen{ 0u };
		if (!FillUnixAddress(this->path, bindAddr, bindAddrLen))
		{
			MS_THROW_TYPE_ERROR("invalid proxy-worker UDS path");
		}

		this->fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (this->fd < 0)
		{
			MS_THROW_ERROR("socket(AF_UNIX, SOCK_DGRAM) failed: %s", std::strerror(errno));
		}
		if (SetNonBlocking(this->fd) != 0)
		{
			const auto err = errno;
			this->Close();
			MS_THROW_ERROR("fcntl(O_NONBLOCK) failed: %s", std::strerror(err));
		}

		(void)::unlink(this->path.c_str());
		if (::bind(this->fd, reinterpret_cast<const sockaddr*>(&bindAddr), bindAddrLen) != 0)
		{
			const auto err = errno;
			this->Close();
			MS_THROW_ERROR("bind(%s) failed: %s", this->path.c_str(), std::strerror(err));
		}

		this->pollHandle       = new uv_poll_t;
		this->pollHandle->data = static_cast<void*>(this);

		int uvErr = uv_poll_init(DepLibUV::GetLoop(), this->pollHandle, this->fd);
		if (uvErr != 0)
		{
			delete this->pollHandle;
			this->pollHandle = nullptr;
			this->Close();
			MS_THROW_ERROR("uv_poll_init() failed: %s", uv_strerror(uvErr));
		}

		uvErr = uv_poll_start(this->pollHandle, UV_READABLE, static_cast<uv_poll_cb>(OnUvPollEvent));
		if (uvErr != 0)
		{
			this->Close();
			MS_THROW_ERROR("uv_poll_start() failed: %s", uv_strerror(uvErr));
		}
	}

	ProxyWorkerSocket::~ProxyWorkerSocket()
	{
		MS_TRACE();

		this->Close();
	}

	void ProxyWorkerSocket::Close() noexcept
	{
		if (this->closing)
		{
			return;
		}
		this->closing = true;
		this->FailAllPendingSends();
		this->sendExpiryTimer.reset();
		if (this->pollHandle)
		{
			uv_poll_stop(this->pollHandle);
			if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(this->pollHandle)))
			{
				uv_close(reinterpret_cast<uv_handle_t*>(this->pollHandle), OnUvClose);
			}
			this->pollHandle = nullptr;
		}

		if (this->fd >= 0)
		{
			::close(this->fd);
			this->fd = -1;
		}

		if (!this->path.empty())
		{
			(void)::unlink(this->path.c_str());
		}

		this->peers.clear();
		this->receiveBatchStorage.reset();
	}

	void ProxyWorkerSocket::OnUvPoll(int status, int events)
	{
		MS_TRACE();

		if (status < 0)
		{
			++this->pollErrors;
			MS_ERROR(
			  "proxy-worker UDS poll error [errors:%llu error:%s]",
			  static_cast<unsigned long long>(this->pollErrors),
			  uv_strerror(status));
			this->Close();
			return;
		}
		if ((events & UV_READABLE) != 0)
		{
			this->ReadPendingDatagrams();
		}
		if ((events & UV_WRITABLE) != 0)
		{
			this->FlushPendingSends();
		}
	}

	void ProxyWorkerSocket::OnTimer(TimerHandle* timer)
	{
		if (this->closing || timer != this->sendExpiryTimer.get())
		{
			return;
		}
		this->FlushPendingSends();
	}

	void ProxyWorkerSocket::ReadPendingDatagrams()
	{
		if (this->fd < 0 || !this->receiveBatchStorage || this->maxReceiveDatagramsPerPoll == 0u)
		{
			return;
		}

		const auto startedAt = std::chrono::steady_clock::now();
		size_t processed{ 0u };
		while (this->fd >= 0 && processed < this->maxReceiveDatagramsPerPoll)
		{
			const auto batchSize = std::min(ReceiveBatchSize, this->maxReceiveDatagramsPerPoll - processed);
			this->receiveBatchStorage->Prepare(batchSize);
			int received{ 0 };
			while (true)
			{
			#ifdef MS_TEST
				if (this->receiveFailuresForTesting > 0u)
				{
					--this->receiveFailuresForTesting;
					errno    = this->receiveErrorForTesting;
					received = -1;
				}
				else
			#endif
				{
					received = ::recvmmsg(
					  this->fd,
					  this->receiveBatchStorage->messages.data(),
					  static_cast<unsigned int>(batchSize),
					  MSG_DONTWAIT,
					  nullptr);
				}
				if (received < 0 && errno == EINTR)
				{
					continue;
				}
				break;
			}
			if (received < 0)
			{
				if (errno != EAGAIN && errno != EWOULDBLOCK)
				{
					const auto error = errno;
					++this->receiveErrors;
					if (ShouldLogDropCounter(this->receiveErrors))
					{
						MS_ERROR(
						  "proxy-worker UDS receive error [errors:%llu error:%s]",
						  static_cast<unsigned long long>(this->receiveErrors),
						  std::strerror(error));
					}
				}
				return;
			}
			if (received == 0)
			{
				return;
			}
			for (int idx{ 0 }; idx < received; ++idx)
			{
				const auto index    = static_cast<size_t>(idx);
				const auto& message = this->receiveBatchStorage->messages[index];
				if ((message.msg_hdr.msg_flags & MSG_TRUNC) != 0)
				{
					++this->truncatedFrameDrops;
					if (ShouldLogDropCounter(this->truncatedFrameDrops))
					{
						MS_ERROR(
						  "proxy-worker UDS truncated frame drop [drops:%llu receivedBytes:%u maxFrameBytes:%zu]",
						  static_cast<unsigned long long>(this->truncatedFrameDrops),
						  message.msg_len,
						  RTC::ProxyWorkerIpc::kMaxFrameSize);
					}
					continue;
				}
				this->ProcessReceivedDatagram(
				  this->receiveBatchStorage->buffers[index].data(),
				  static_cast<size_t>(message.msg_len),
				  this->receiveBatchStorage->peerAddrs[index],
				  message.msg_hdr.msg_namelen);
			}
			processed += static_cast<size_t>(received);
			if (static_cast<size_t>(received) < batchSize)
			{
				return;
			}
			if (std::chrono::steady_clock::now() - startedAt >=
			    std::chrono::milliseconds(this->maxReceiveWorkPerPollMs))
			{
				++this->receiveBudgetYields;
				return;
			}
		}
		++this->receiveBudgetYields;
	}

	void ProxyWorkerSocket::ProcessReceivedDatagram(
	  const uint8_t* data, size_t len, const sockaddr_un& peerAddr, socklen_t peerAddrLen)
	{
		if (!data || len == 0u)
		{
			++this->malformedFrameDrops;
			if (ShouldLogDropCounter(this->malformedFrameDrops))
			{
				MS_ERROR(
				  "proxy-worker UDS malformed frame drop [drops:%llu error:empty_frame]",
				  static_cast<unsigned long long>(this->malformedFrameDrops));
			}
			return;
		}
		RTC::ProxyWorkerIpc::DecodedFrame frame;
		const auto decodeError = RTC::ProxyWorkerIpc::DecodeFrame(data, len, frame);
		if (decodeError != RTC::ProxyWorkerIpc::DecodeError::None)
		{
			++this->malformedFrameDrops;
			if (ShouldLogDropCounter(this->malformedFrameDrops))
			{
				MS_ERROR(
				  "proxy-worker UDS malformed frame drop [drops:%llu error:%u]",
				  static_cast<unsigned long long>(this->malformedFrameDrops),
				  static_cast<unsigned>(decodeError));
			}
			return;
		}

		this->recvBytes += frame.payloadLen;
		this->RememberPeer(reinterpret_cast<const sockaddr*>(&frame.remoteAddr), peerAddr, peerAddrLen);
		this->listener->OnProxyWorkerSocketPacketReceived(
		  this, frame.payload, frame.payloadLen, reinterpret_cast<const sockaddr*>(&frame.remoteAddr));
	}

	std::string ProxyWorkerSocket::RemoteAddressKey(const struct sockaddr* remoteAddr) const
	{
		size_t len{ 0u };
		if (!RTC::ProxyWorkerIpc::SockaddrLength(remoteAddr, len))
		{
			return {};
		}

		return std::string(reinterpret_cast<const char*>(remoteAddr), len);
	}

	void ProxyWorkerSocket::RememberPeer(
	  const struct sockaddr* remoteAddr, const sockaddr_un& peerAddr, socklen_t peerLen)
	{
		const auto key = this->RemoteAddressKey(remoteAddr);
		if (key.empty() || peerLen == 0u)
		{
			return;
		}

		PeerAddress peer;
		peer.addr        = peerAddr;
		peer.len         = peerLen;
		this->peers[key] = peer;
	}

	void ProxyWorkerSocket::Send(
	  const uint8_t* data, size_t len, const struct sockaddr* remoteAddr, onSendCallback* cb)
	{
		MS_TRACE();

		bool sent{ false };
		bool callbackOwnedByQueue{ false };
		if (!this->closing && this->fd >= 0 && data && len > 0u)
		{
			const auto key = this->RemoteAddressKey(remoteAddr);
			const auto it  = key.empty() ? this->peers.end() : this->peers.find(key);
			if (it != this->peers.end())
			{
				if (!this->pendingSends.empty())
				{
					callbackOwnedByQueue = this->EnqueuePendingSend(data, len, it->second, cb);
				}
				else
				{
					bool transientFailure{ false };
					sent = this->TrySendNow(data, len, it->second, transientFailure);
					if (!sent && transientFailure)
					{
						callbackOwnedByQueue = this->EnqueuePendingSend(data, len, it->second, cb);
					}
				}
			}
			else
			{
				++this->unknownPeerDrops;
				if (ShouldLogDropCounter(this->unknownPeerDrops))
				{
					MS_ERROR(
					  "proxy-worker UDS unknown peer drop [drops:%llu pendingDatagrams:%zu pendingBytes:%zu datagramBytes:%zu]",
					  static_cast<unsigned long long>(this->unknownPeerDrops),
					  this->pendingSends.size(),
					  this->pendingSendBytes,
					  len);
				}
			}
		}

		if (cb && !callbackOwnedByQueue)
		{
			try
			{
				(*cb)(sent);
			}
			catch (...)
			{
			}
			delete cb;
		}
	}

	bool ProxyWorkerSocket::TrySendNow(
	  const uint8_t* data, size_t len, const PeerAddress& peer, bool& transientFailure)
	{
		transientFailure = false;
		while (this->fd >= 0)
		{
			ssize_t nsent{ -1 };
#ifdef MS_TEST
			if (this->shortSendsForTesting > 0u)
			{
				--this->shortSendsForTesting;
				nsent = len > 0u ? static_cast<ssize_t>(len - 1u) : 0;
			}
			else if (this->sendFailuresForTesting > 0u)
			{
				--this->sendFailuresForTesting;
				errno = this->sendErrorForTesting;
			}
			else
#endif
			{
				nsent =
				  ::sendto(this->fd, data, len, 0, reinterpret_cast<const sockaddr*>(&peer.addr), peer.len);
			}
			if (nsent >= 0)
			{
				this->sentBytes += static_cast<size_t>(nsent);
				if (static_cast<size_t>(nsent) == len)
				{
					return true;
				}
				++this->shortSendErrors;
				if (ShouldLogDropCounter(this->shortSendErrors))
				{
					MS_ERROR(
					  "proxy-worker UDS short send [errors:%llu sentBytes:%zd datagramBytes:%zu]",
					  static_cast<unsigned long long>(this->shortSendErrors),
					  nsent,
					  len);
				}
				return false;
			}
			if (errno == EINTR)
			{
				continue;
			}
			if (IsTransientSendError(errno))
			{
				transientFailure = true;
				++this->transientSendRetries;
				return false;
			}
			const auto error = errno;
			++this->hardSendErrors;
			if (ShouldLogDropCounter(this->hardSendErrors))
			{
				MS_ERROR(
				  "proxy-worker UDS hard send error [errors:%llu pendingDatagrams:%zu pendingBytes:%zu error:%s]",
				  static_cast<unsigned long long>(this->hardSendErrors),
				  this->pendingSends.size(),
				  this->pendingSendBytes,
				  std::strerror(error));
			}
			return false;
		}
		return false;
	}

	bool ProxyWorkerSocket::EnqueuePendingSend(
	  const uint8_t* data, size_t len, const PeerAddress& peer, onSendCallback* cb)
	{
		if (
		  !data || len == 0u || this->pendingSends.size() >= this->maxPendingSendDatagrams ||
		  len > this->maxPendingSendBytes - std::min(this->pendingSendBytes, this->maxPendingSendBytes))
		{
			++this->sendQueueFullDrops;
			if (ShouldLogDropCounter(this->sendQueueFullDrops))
			{
				MS_ERROR(
				  "proxy-worker UDS send queue full drop [drops:%llu pendingDatagrams:%zu pendingBytes:%zu datagramBytes:%zu]",
				  static_cast<unsigned long long>(this->sendQueueFullDrops),
				  this->pendingSends.size(),
				  this->pendingSendBytes,
				  len);
			}
			return false;
		}
		try
		{
			PendingSend pending;
			pending.data.assign(data, data + len);
			pending.peer       = peer;
			pending.cb         = cb;
			pending.enqueuedAt = std::chrono::steady_clock::now();
			this->pendingSends.emplace_back(std::move(pending));
			this->pendingSendBytes += len;
		}
		catch (...)
		{
			++this->sendQueueFullDrops;
			if (ShouldLogDropCounter(this->sendQueueFullDrops))
			{
				MS_ERROR(
				  "proxy-worker UDS send queue allocation drop [drops:%llu pendingDatagrams:%zu pendingBytes:%zu datagramBytes:%zu]",
				  static_cast<unsigned long long>(this->sendQueueFullDrops),
				  this->pendingSends.size(),
				  this->pendingSendBytes,
				  len);
			}
			return false;
		}
		if (!this->UpdatePendingSendWatchers())
		{
			this->FailAllPendingSends();
			this->Close();
			// The queue took ownership before poll reconfiguration. Its failure
			// path already completed and deleted the callback.
			return true;
		}
		return true;
	}

	void ProxyWorkerSocket::FlushPendingSends()
	{
		const auto startedAt = std::chrono::steady_clock::now();
		size_t processed{ 0u };
		while (!this->pendingSends.empty() && processed < MaxSendDatagramsPerPoll)
		{
			auto& pending    = this->pendingSends.front();
			const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			                     std::chrono::steady_clock::now() - pending.enqueuedAt)
			                     .count();
			if (ageMs >= static_cast<int64_t>(this->maxPendingSendAgeMs))
			{
				++this->sendExpiredDrops;
				if (ShouldLogDropCounter(this->sendExpiredDrops))
				{
					MS_ERROR(
					  "proxy-worker UDS expired send drop [drops:%llu ageMs:%lld pendingDatagrams:%zu pendingBytes:%zu]",
					  static_cast<unsigned long long>(this->sendExpiredDrops),
					  static_cast<long long>(ageMs),
					  this->pendingSends.size(),
					  this->pendingSendBytes);
				}
				this->CompletePendingSend(pending, false);
				this->pendingSends.pop_front();
				++processed;
				continue;
			}
			bool transientFailure{ false };
			const bool sent =
			  this->TrySendNow(pending.data.data(), pending.data.size(), pending.peer, transientFailure);
			if (transientFailure)
			{
				break;
			}
			this->CompletePendingSend(pending, sent);
			this->pendingSends.pop_front();
			++processed;
			if (std::chrono::steady_clock::now() - startedAt >= MaxSendIoWorkPerPoll)
			{
				break;
			}
		}
		if (!this->UpdatePendingSendWatchers())
		{
			this->FailAllPendingSends();
			this->Close();
		}
	}

	void ProxyWorkerSocket::CompletePendingSend(PendingSend& pending, bool sent) noexcept
	{
		this->pendingSendBytes = this->pendingSendBytes >= pending.data.size()
		                           ? this->pendingSendBytes - pending.data.size()
		                           : 0u;
		if (pending.cb)
		{
			try
			{
				(*pending.cb)(sent);
			}
			catch (...)
			{
			}
			delete pending.cb;
			pending.cb = nullptr;
		}
	}

	void ProxyWorkerSocket::FailAllPendingSends() noexcept
	{
		while (!this->pendingSends.empty())
		{
			auto& pending = this->pendingSends.front();
			this->CompletePendingSend(pending, false);
			this->pendingSends.pop_front();
		}
		this->pendingSendBytes = 0u;
	}

	bool ProxyWorkerSocket::UpdatePollEvents() noexcept
	{
		if (!this->pollHandle || this->fd < 0)
		{
			return false;
		}
		const int events = this->pendingSends.empty() ? UV_READABLE : UV_READABLE | UV_WRITABLE;
		int err{ 0 };
	#ifdef MS_TEST
		if (this->watcherFailuresForTesting > 0u)
		{
			--this->watcherFailuresForTesting;
			err = UV_EINVAL;
		}
		else
	#endif
		{
			err = uv_poll_start(this->pollHandle, events, static_cast<uv_poll_cb>(OnUvPollEvent));
		}
		if (err != 0)
		{
			++this->watcherErrors;
			MS_ERROR(
			  "proxy-worker UDS poll watcher update failed [errors:%llu error:%s]",
			  static_cast<unsigned long long>(this->watcherErrors),
			  uv_strerror(err));
			return false;
		}
		return true;
	}

	bool ProxyWorkerSocket::UpdateExpiryTimer() noexcept
	{
		if (!this->sendExpiryTimer)
		{
			return false;
		}
		try
		{
			if (this->pendingSends.empty())
			{
				if (this->sendExpiryTimer->IsActive())
				{
					this->sendExpiryTimer->Stop();
				}
				return true;
			}

			const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			                     std::chrono::steady_clock::now() -
			                     this->pendingSends.front().enqueuedAt)
			                     .count();
			const auto remainingMs =
			  ageMs >= static_cast<int64_t>(this->maxPendingSendAgeMs)
			    ? 1u
			    : std::max<uint64_t>(
			        1u, this->maxPendingSendAgeMs - static_cast<uint64_t>(ageMs));
			this->sendExpiryTimer->Start(remainingMs);
			return true;
		}
		catch (const std::exception& error)
		{
			++this->watcherErrors;
			MS_ERROR(
			  "proxy-worker UDS expiry timer update failed [errors:%llu error:%s]",
			  static_cast<unsigned long long>(this->watcherErrors),
			  error.what());
		}
		catch (...)
		{
			++this->watcherErrors;
			MS_ERROR(
			  "proxy-worker UDS expiry timer update failed [errors:%llu error:unknown]",
			  static_cast<unsigned long long>(this->watcherErrors));
		}
		return false;
	}

	bool ProxyWorkerSocket::UpdatePendingSendWatchers() noexcept
	{
		return this->UpdatePollEvents() && this->UpdateExpiryTimer();
	}
} // namespace RTC

#define MS_CLASS "RTC::ProxyWorkerSocket"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/ProxyWorkerSocket.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "RTC/ProxyWorkerIpc.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace RTC
{
	namespace
	{
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
	}

	void ProxyWorkerSocket::OnUvPoll(int status, int events)
	{
		MS_TRACE();

		if (status < 0)
		{
			MS_WARN_DEV("proxy-worker UDS poll error: %s", uv_strerror(status));
			return;
		}
		if ((events & UV_READABLE) == 0)
		{
			return;
		}

		this->ReadPendingDatagrams();
	}

	void ProxyWorkerSocket::ReadPendingDatagrams()
	{
		std::array<uint8_t, RTC::ProxyWorkerIpc::kMaxFrameSize> buffer{};

		while (this->fd >= 0)
		{
			sockaddr_un peerAddr{};
			socklen_t peerAddrLen = sizeof(peerAddr);
			const ssize_t nread   = ::recvfrom(
                this->fd,
                buffer.data(),
                buffer.size(),
                0,
                reinterpret_cast<sockaddr*>(&peerAddr),
                &peerAddrLen);

			if (nread < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					return;
				}
				if (errno == EINTR)
				{
					continue;
				}
				MS_WARN_DEV("proxy-worker UDS recvfrom() failed: %s", std::strerror(errno));
				return;
			}
			if (nread == 0)
			{
				continue;
			}

			RTC::ProxyWorkerIpc::DecodedFrame frame;
			const auto decodeError = RTC::ProxyWorkerIpc::DecodeFrame(
			  buffer.data(), static_cast<size_t>(nread), frame);

			if (decodeError != RTC::ProxyWorkerIpc::DecodeError::None)
			{
				MS_WARN_DEV(
				  "proxy-worker UDS dropped malformed frame [error:%u]",
				  static_cast<unsigned>(decodeError));
				continue;
			}

			this->recvBytes += frame.payloadLen;
			this->RememberPeer(
			  reinterpret_cast<const sockaddr*>(&frame.remoteAddr), peerAddr, peerAddrLen);
			this->listener->OnProxyWorkerSocketPacketReceived(
			  this,
			  frame.payload,
			  frame.payloadLen,
			  reinterpret_cast<const sockaddr*>(&frame.remoteAddr));
		}
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
		peer.addr = peerAddr;
		peer.len  = peerLen;
		this->peers[key] = peer;
	}

	void ProxyWorkerSocket::Send(
	  const uint8_t* data, size_t len, const struct sockaddr* remoteAddr, onSendCallback* cb)
	{
		MS_TRACE();

		bool sent{ false };
		if (this->fd >= 0 && data && len > 0u)
		{
			const auto key = this->RemoteAddressKey(remoteAddr);
			const auto it  = key.empty() ? this->peers.end() : this->peers.find(key);
			if (it != this->peers.end())
			{
				while (true)
				{
					const ssize_t nsent = ::sendto(
					  this->fd,
					  data,
					  len,
					  0,
					  reinterpret_cast<const sockaddr*>(&it->second.addr),
					  it->second.len);

					if (nsent >= 0)
					{
						this->sentBytes += static_cast<size_t>(nsent);
						sent = static_cast<size_t>(nsent) == len;
						break;
					}
					if (errno == EINTR)
					{
						continue;
					}
					MS_WARN_DEV("proxy-worker UDS sendto() failed: %s", std::strerror(errno));
					break;
				}
			}
		}

		if (cb)
		{
			(*cb)(sent);
			delete cb;
		}
	}
} // namespace RTC

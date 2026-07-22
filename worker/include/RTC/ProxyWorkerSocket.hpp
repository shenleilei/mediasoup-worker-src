#ifndef MS_RTC_PROXY_WORKER_SOCKET_HPP
#define MS_RTC_PROXY_WORKER_SOCKET_HPP

#include "common.hpp"
#include <functional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unordered_map>
#include <uv.h>

namespace RTC
{
	class ProxyWorkerSocket
	{
	public:
		using onSendCallback = const std::function<void(bool sent)>;

	public:
		class Listener
		{
		public:
			virtual ~Listener() = default;

		public:
			virtual void OnProxyWorkerSocketPacketReceived(
			  RTC::ProxyWorkerSocket* socket,
			  const uint8_t* data,
			  size_t len,
			  const struct sockaddr* remoteAddr) = 0;
		};

	public:
		ProxyWorkerSocket(Listener* listener, std::string path, const struct sockaddr* localAddress);
		~ProxyWorkerSocket();

	public:
		void Send(
		  const uint8_t* data, size_t len, const struct sockaddr* remoteAddr, onSendCallback* cb = nullptr);

		const std::string& GetPath() const
		{
			return this->path;
		}

		const struct sockaddr* GetLocalAddress() const
		{
			return reinterpret_cast<const struct sockaddr*>(&this->localAddr);
		}

		size_t GetRecvBytes() const
		{
			return this->recvBytes;
		}

		size_t GetSentBytes() const
		{
			return this->sentBytes;
		}

	public:
		void OnUvPoll(int status, int events);

	private:
		struct PeerAddress
		{
			sockaddr_un addr{};
			socklen_t len{ 0u };
		};

	private:
		void Close() noexcept;
		void ReadPendingDatagrams();
		std::string RemoteAddressKey(const struct sockaddr* remoteAddr) const;
		void RememberPeer(const struct sockaddr* remoteAddr, const sockaddr_un& peerAddr, socklen_t peerLen);

	private:
		Listener* listener{ nullptr };
		std::string path;
		int fd{ -1 };
		uv_poll_t* pollHandle{ nullptr };
		sockaddr_storage localAddr{};
		std::unordered_map<std::string, PeerAddress> peers;
		size_t recvBytes{ 0u };
		size_t sentBytes{ 0u };
	};
} // namespace RTC

#endif

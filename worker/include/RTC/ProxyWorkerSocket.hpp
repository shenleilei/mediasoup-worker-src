#ifndef MS_RTC_PROXY_WORKER_SOCKET_HPP
#define MS_RTC_PROXY_WORKER_SOCKET_HPP

#include "common.hpp"
#include "handles/TimerHandle.hpp"
#include <uv.h>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unordered_map>
#include <vector>

namespace RTC
{
	class ProxyWorkerSocket : public TimerHandle::Listener
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
		static constexpr size_t MaxPendingSendDatagrams{ 4096u };
		static constexpr size_t MaxPendingSendBytes{ 8u * 1024u * 1024u };
		static constexpr uint64_t MaxPendingSendAgeMs{ 250u };
		static constexpr size_t MaxReceiveDatagramsPerPoll{ 128u };

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

		size_t GetPendingSendDatagrams() const
		{
			return this->pendingSends.size();
		}

		size_t GetPendingSendBytes() const
		{
			return this->pendingSendBytes;
		}

		uint64_t GetTransientSendRetries() const
		{
			return this->transientSendRetries;
		}

		uint64_t GetSendQueueFullDrops() const
		{
			return this->sendQueueFullDrops;
		}

		uint64_t GetSendExpiredDrops() const
		{
			return this->sendExpiredDrops;
		}

		uint64_t GetHardSendErrors() const
		{
			return this->hardSendErrors;
		}

		uint64_t GetReceiveBudgetYields() const
		{
			return this->receiveBudgetYields;
		}

#ifdef MS_TEST
		void FailNextSendsForTesting(int error, size_t attempts)
		{
			this->sendErrorForTesting    = error;
			this->sendFailuresForTesting = attempts;
		}

		void SetSendQueueLimitsForTesting(size_t datagrams, size_t bytes)
		{
			this->maxPendingSendDatagrams = datagrams;
			this->maxPendingSendBytes     = bytes;
		}

		void SetMaxPendingSendAgeForTesting(uint64_t ageMs)
		{
			this->maxPendingSendAgeMs = ageMs;
		}

		void SetReceiveBudgetForTesting(size_t datagrams)
		{
			this->maxReceiveDatagramsPerPoll = datagrams;
		}
#endif

	public:
		void OnUvPoll(int status, int events);
		void OnTimer(TimerHandle* timer) override;

	private:
		struct ReceiveBatchStorage;

		struct PeerAddress
		{
			sockaddr_un addr{};
			socklen_t len{ 0u };
		};

		struct PendingSend
		{
			std::vector<uint8_t> data;
			PeerAddress peer;
			onSendCallback* cb{ nullptr };
			std::chrono::steady_clock::time_point enqueuedAt;
		};

	private:
		void Close() noexcept;
		void ReadPendingDatagrams();
		void ProcessReceivedDatagram(
		  const uint8_t* data, size_t len, const sockaddr_un& peerAddr, socklen_t peerAddrLen);
		bool TrySendNow(const uint8_t* data, size_t len, const PeerAddress& peer, bool& transientFailure);
		bool EnqueuePendingSend(
		  const uint8_t* data, size_t len, const PeerAddress& peer, onSendCallback* cb);
		void FlushPendingSends();
		void CompletePendingSend(PendingSend& pending, bool sent) noexcept;
		void FailAllPendingSends() noexcept;
		bool UpdatePollEvents() noexcept;
		bool UpdateExpiryTimer() noexcept;
		bool UpdatePendingSendWatchers() noexcept;
		std::string RemoteAddressKey(const struct sockaddr* remoteAddr) const;
		void RememberPeer(const struct sockaddr* remoteAddr, const sockaddr_un& peerAddr, socklen_t peerLen);

	private:
		Listener* listener{ nullptr };
		std::string path;
		int fd{ -1 };
		uv_poll_t* pollHandle{ nullptr };
		sockaddr_storage localAddr{};
		std::unordered_map<std::string, PeerAddress> peers;
		std::unique_ptr<ReceiveBatchStorage> receiveBatchStorage;
		std::unique_ptr<TimerHandle> sendExpiryTimer;
		std::deque<PendingSend> pendingSends;
		size_t pendingSendBytes{ 0u };
		size_t maxPendingSendDatagrams{ MaxPendingSendDatagrams };
		size_t maxPendingSendBytes{ MaxPendingSendBytes };
		uint64_t maxPendingSendAgeMs{ MaxPendingSendAgeMs };
		size_t maxReceiveDatagramsPerPoll{ MaxReceiveDatagramsPerPoll };
		size_t recvBytes{ 0u };
		size_t sentBytes{ 0u };
		uint64_t transientSendRetries{ 0u };
		uint64_t sendQueueFullDrops{ 0u };
		uint64_t sendExpiredDrops{ 0u };
		uint64_t hardSendErrors{ 0u };
		uint64_t receiveBudgetYields{ 0u };
		bool closing{ false };
#ifdef MS_TEST
		int sendErrorForTesting{ 0 };
		size_t sendFailuresForTesting{ 0u };
#endif
	};
} // namespace RTC

#endif

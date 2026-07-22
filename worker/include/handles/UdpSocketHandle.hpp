#ifndef MS_UDP_SOCKET_HANDLE_HPP
#define MS_UDP_SOCKET_HANDLE_HPP

#include "common.hpp"
#include <uv.h>
#include <array>
#include <deque>
#include <string>

class UdpSocketHandle
{
protected:
	using onSendCallback = const std::function<void(bool sent)>;

public:
	/* Struct for the data field of uv_req_t when sending a datagram. */
	struct UvSendData
	{
		explicit UvSendData(size_t storeSize) : store(new uint8_t[storeSize])
		{
		}

		// Disable copy constructor because of the dynamically allocated data (store).
		UvSendData(const UvSendData&) = delete;

		~UvSendData()
		{
			delete[] this->store;
			delete this->cb;
		}

		uv_udp_send_t req{};
		uint8_t* store{ nullptr };
		UdpSocketHandle::onSendCallback* cb{ nullptr };
	};

#ifdef __linux__
	struct PendingSendData
	{
		static constexpr size_t InlineCapacity{ 2048u };

		PendingSendData() = default;
		PendingSendData(const PendingSendData&) = delete;
		PendingSendData& operator=(const PendingSendData&) = delete;
		PendingSendData(PendingSendData&&) noexcept = default;
		PendingSendData& operator=(PendingSendData&&) noexcept = default;

		uint8_t* Data()
		{
			return this->heapStore ? this->heapStore.get() : this->inlineStore.data();
		}

		const uint8_t* Data() const
		{
			return this->heapStore ? this->heapStore.get() : this->inlineStore.data();
		}

		struct sockaddr_storage addr
		{
		};
		socklen_t addrLen{ 0u };
		size_t len{ 0u };
		std::array<uint8_t, InlineCapacity> inlineStore{};
		std::unique_ptr<uint8_t[]> heapStore;
		UdpSocketHandle::onSendCallback* cb{ nullptr };
	};
#endif

public:
	/**
	 * uvHandle must be an already initialized and binded uv_udp_t pointer.
	 */
	explicit UdpSocketHandle(uv_udp_t* uvHandle);
	UdpSocketHandle& operator=(const UdpSocketHandle&) = delete;
	UdpSocketHandle(const UdpSocketHandle&)            = delete;
	virtual ~UdpSocketHandle();

public:
	bool IsClosed() const
	{
		return this->closed;
	}
	void Dump() const;
	void Send(
	  const uint8_t* data, size_t len, const struct sockaddr* addr, UdpSocketHandle::onSendCallback* cb);
	const struct sockaddr* GetLocalAddress() const
	{
		return reinterpret_cast<const struct sockaddr*>(&this->localAddr);
	}
	int GetLocalFamily() const
	{
		return reinterpret_cast<const struct sockaddr*>(&this->localAddr)->sa_family;
	}
	const std::string& GetLocalIp() const
	{
		return this->localIp;
	}
	uint16_t GetLocalPort() const
	{
		return this->localPort;
	}
	size_t GetRecvBytes() const
	{
		return this->recvBytes;
	}
	size_t GetSentBytes() const
	{
		return this->sentBytes;
	}
	uint32_t GetSendBufferSize() const;
	void SetSendBufferSize(uint32_t size);
	uint32_t GetRecvBufferSize() const;
	void SetRecvBufferSize(uint32_t size);

#ifdef MS_TEST
	static void FailNextFilenoForTesting();
	static size_t GetReadBufferSizeForTesting();
#endif

private:
	void InternalClose() noexcept;
	bool SetLocalAddress();
	void SendImmediate(
	  const uint8_t* data, size_t len, const struct sockaddr* addr, UdpSocketHandle::onSendCallback* cb);

#ifdef __linux__
	bool EnqueueBatchedSend(
	  const uint8_t* data, size_t len, const struct sockaddr* addr, UdpSocketHandle::onSendCallback* cb);
	void StartBatchedSendCheck();
	void StopBatchedSendCheck();
	void FlushBatchedSends();
	void FlushBatchedSendsWithImmediateFallback();
	void FailPendingBatchedSends();
#endif

	/* Callbacks fired by UV events. */
public:
	void OnUvRecvAlloc(size_t suggestedSize, uv_buf_t* buf);
	void OnUvRecv(ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags);
	void OnUvSend(int status, UdpSocketHandle::onSendCallback* cb);
#ifdef __linux__
	void OnUvBatchedSendCheck();
#endif

	/* Pure virtual methods that must be implemented by the subclass. */
protected:
	virtual void UserOnUdpDatagramReceived(
	  const uint8_t* data, size_t len, const struct sockaddr* addr) = 0;

protected:
	struct sockaddr_storage localAddr
	{
	};
	std::string localIp;
	uint16_t localPort{ 0u };

private:
	// Allocated by this (may be passed by argument).
	uv_udp_t* uvHandle{ nullptr };
	// Others.
#if defined(MS_LIBURING_SUPPORTED) || defined(__linux__)
	// Local file descriptor for io_uring.
	uv_os_fd_t fd{ -1 };
#endif
#ifdef __linux__
	uv_check_t* batchedSendCheckHandle{ nullptr };
	std::deque<PendingSendData> pendingBatchedSends;
	size_t sendmmsgBatchSize{ 0u };
	bool batchedSendCheckActive{ false };
	bool flushingBatchedSends{ false };
#endif
	bool closed{ false };
	size_t recvBytes{ 0u };
	size_t sentBytes{ 0u };
};

#endif

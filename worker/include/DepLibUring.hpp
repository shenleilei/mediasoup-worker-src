#ifndef MS_DEP_LIBURING_HPP
#define MS_DEP_LIBURING_HPP

#include "DepLibUV.hpp"
#include "DepLibUringBufferLimits.hpp"
#include "DepLibUringPolicy.hpp"
#include "FBS/liburing.h"
#include <array>
#include <functional>
#include <liburing.h>

class DepLibUring
{
public:
	using onSendCallback = const std::function<void(bool sent)>;
	static constexpr size_t QueueDepth{ DepLibUringBufferLimits::QueueDepth };
	static constexpr size_t SendBufferSize{ DepLibUringBufferLimits::SendBufferSize };
	static constexpr size_t FrameLenSize{ DepLibUringBufferLimits::FrameLenSize };

	static constexpr bool CanStoreSendData(size_t len) noexcept
	{
		return DepLibUringBufferLimits::CanStoreSendData(len);
	}
	static constexpr bool CanStoreSendDataWithTrailer(size_t len, size_t trailerLen) noexcept
	{
		return DepLibUringBufferLimits::CanStoreSendDataWithTrailer(len, trailerLen);
	}
	static constexpr bool CanStoreWriteData(size_t len1, size_t len2, bool data2InSendBuffer) noexcept
	{
		return DepLibUringBufferLimits::CanStoreWriteData(len1, len2, data2InSendBuffer);
	}
	static constexpr bool IsCompleteIoResult(int result, size_t expectedLen) noexcept
	{
		return DepLibUringBufferLimits::IsCompleteIoResult(result, expectedLen);
	}

	/* Struct for the user data field of SQE and CQE. */
	struct UserData
	{
		// Pointer to send buffer.
		uint8_t* store{ nullptr };
		// Frame len buffer for TCP.
		uint8_t frameLen[FrameLenSize] = { 0 };
		// iovec for TCP, first item for framing, second item for payload.
		struct iovec iov[2];
		// Send callback.
		onSendCallback* cb{ nullptr };
		// Index in userDatas array.
		size_t idx{ 0 };
		// Exact datagram or framed-stream byte count expected in the CQE.
		size_t expectedLen{ 0u };
		// Whether this entry is currently reserved by a prepared/in-flight SQE.
		bool inUse{ false };
	};

	using SendBuffer = uint8_t[SendBufferSize];

	static bool IsRuntimeSupported();
	static void ClassInit();
	static void ClassDestroy();
	static flatbuffers::Offset<FBS::LibUring::Dump> FillBuffer(flatbuffers::FlatBufferBuilder& builder);
	static void StartPollingCQEs();
	static void StopPollingCQEs();
	static uint8_t* GetSendBuffer();
	static bool PrepareSend(
	  int sockfd, const uint8_t* data, size_t len, const struct sockaddr* addr, onSendCallback* cb);
	static bool PrepareWrite(
	  int sockfd, const uint8_t* data1, size_t len1, const uint8_t* data2, size_t len2, onSendCallback* cb);
	static void Submit();
	static void SetActive();
	static bool IsActive();

	class LibUring;

	thread_local static LibUring* liburing;

public:
	class LibUring
	{
	public:
		LibUring();
		~LibUring();
		flatbuffers::Offset<FBS::LibUring::Dump> FillBuffer(flatbuffers::FlatBufferBuilder& builder) const;
		void StartPollingCQEs();
		void StopPollingCQEs();
		uint8_t* GetSendBuffer();
		bool PrepareSend(
		  int sockfd, const uint8_t* data, size_t len, const struct sockaddr* addr, onSendCallback* cb);
		bool PrepareWrite(
		  int sockfd,
		  const uint8_t* data1,
		  size_t len1,
		  const uint8_t* data2,
		  size_t len2,
		  onSendCallback* cb);
		void Submit();
		void SetActive()
		{
			if (this->operational)
			{
				this->active = true;
			}
		}
		bool IsActive() const
		{
			return this->active;
		}
		bool IsZeroCopyEnabled() const
		{
			return this->zeroCopyEnabled;
		}
		bool IsOperational() const
		{
			return this->operational;
		}
		// Called only by the libuv completion/submission callbacks when the ring can
		// no longer make safe forward progress.
		void FailClosed(const char* reason) noexcept;
		io_uring* GetRing()
		{
			return std::addressof(this->ring);
		}
		int GetEventFd() const
		{
			return this->efd;
		}
		void ReleaseUserDataEntry(size_t idx);

	private:
		UserData* GetUserData();
		uint8_t* PeekSendBuffer() const noexcept;
		bool ScheduleSubmitRetry() noexcept;
		void ResetSubmitRetry() noexcept;

	private:
		// io_uring instance.
		io_uring ring;
		// Event file descriptor to watch for io_uring completions.
		int efd{ -1 };
		// libuv handle used to poll io_uring completions.
		uv_poll_t* uvHandle{ nullptr };
		// One-shot timer used when the kernel temporarily refuses submission.
		uv_timer_t* submitRetryHandle{ nullptr };
		// Whether we are currently sending RTP over io_uring.
		bool active{ false };
		bool operational{ true };
		bool ringInitialized{ false };
		unsigned int deferredSubmitRetryCount{ 0u };
		// Whether Zero Copy feature is enabled.
		bool zeroCopyEnabled{ true };
		// Pre-allocated UserData's.
		UserData userDatas[QueueDepth]{};
		// Allocation-free ring of available UserData indexes.
		std::array<size_t, QueueDepth> availableUserDataEntries{};
		size_t availableUserDataEntryHead{ 0u };
		size_t availableUserDataEntryCount{ 0u };
		// Pre-allocated SendBuffer's.
		SendBuffer sendBuffers[QueueDepth];
		// iovec structs to be registered for Zero Copy.
		struct iovec iovecs[QueueDepth];
		// Submission queue entry process count.
		uint64_t sqeProcessCount{ 0u };
		// Submission queue entry miss count.
		uint64_t sqeMissCount{ 0u };
		// User data miss count.
		uint64_t userDataMissCount{ 0u };
	};
};

#endif

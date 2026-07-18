#define MS_CLASS "DepLibUring"
// #define MS_LOG_DEV_LEVEL 3

#include "DepLibUring.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/utsname.h>

/* Static variables. */

/* liburing instance per thread. */
thread_local DepLibUring::LibUring* DepLibUring::liburing{ nullptr };
/* Completion queue entry array used to retrieve processes tasks. */
thread_local struct io_uring_cqe* cqes[DepLibUring::QueueDepth];

/* Static methods for UV callbacks. */

inline static void onCloseFd(uv_handle_t* handle)
{
	delete reinterpret_cast<uv_poll_t*>(handle);
}

inline static void onCloseSubmitRetry(uv_handle_t* handle)
{
	delete reinterpret_cast<uv_timer_t*>(handle);
}

inline static void onSubmitRetry(uv_timer_t* handle)
{
	auto* liburing = static_cast<DepLibUring::LibUring*>(handle->data);

	if (liburing && liburing->IsOperational())
	{
		try
		{
			liburing->Submit();
		}
		catch (...)
		{
			liburing->FailClosed("exception while retrying io_uring submission");
		}
	}
}

inline static void settleSendCallbackNoThrow(DepLibUring::onSendCallback*& cb, bool sent) noexcept
{
	if (DepLibUringPolicy::SettleCallbackNoThrow(cb, sent) == DepLibUringPolicy::CallbackSettlement::Threw)
	{
		try
		{
			MS_ERROR("io_uring completion callback failed with an exception");
		}
		catch (...)
		{
		}
	}
}

inline static void onFdEvent(uv_poll_t* handle, int status, int events)
{
	auto* liburing = static_cast<DepLibUring::LibUring*>(handle->data);

	if (!liburing)
	{
		return;
	}
	if (!liburing->IsOperational())
	{
		return;
	}

	if (status < 0)
	{
		liburing->FailClosed(uv_strerror(status));

		return;
	}

	if ((events & UV_READABLE) == 0)
	{
		return;
	}

	// libuv uses level triggering, so we need to read from the socket to reset
	// the counter in order to avoid libuv calling this callback indefinitely. Read
	// first: completions that arrive afterwards either appear in the drain below or
	// increment eventfd for a subsequent callback.
	eventfd_t v;
	int err;
	do
	{
		err = eventfd_read(liburing->GetEventFd(), std::addressof(v));
	} while (err < 0 && errno == EINTR);

	if (err < 0 && errno != EAGAIN)
	{
		const int error = errno;

		liburing->FailClosed(std::strerror(error));

		return;
	}

	while (true)
	{
		auto count = io_uring_peek_batch_cqe(liburing->GetRing(), cqes, DepLibUring::QueueDepth);

		if (count == 0u)
		{
			break;
		}

		for (unsigned int i{ 0 }; i < count; ++i)
		{
			struct io_uring_cqe* cqe = cqes[i];
			auto* userData           = static_cast<DepLibUring::UserData*>(io_uring_cqe_get_data(cqe));

			if (!userData)
			{
				io_uring_cqe_seen(liburing->GetRing(), cqe);
				liburing->FailClosed("completion did not contain user data");

				return;
			}

			if (!userData->inUse)
			{
				io_uring_cqe_seen(liburing->GetRing(), cqe);
				liburing->FailClosed("completion referenced an already released user data entry");

				return;
			}

			const auto action = DepLibUringPolicy::ClassifyCompletion(
			  liburing->IsZeroCopyEnabled(),
			  (cqe->flags & IORING_CQE_F_NOTIF) != 0,
			  (cqe->flags & IORING_CQE_F_MORE) != 0,
			  cqe->res,
			  userData->expectedLen);

			auto* cb = DepLibUringPolicy::TakeCompletionCallback(action, userData->cb);

			// Finalize CQE and slot ownership before invoking arbitrary application
			// code. A zero-copy result carrying MORE retains the slot until NOTIF.
			if (action.releaseUserData)
			{
				liburing->ReleaseUserDataEntry(userData->idx);
			}
			io_uring_cqe_seen(liburing->GetRing(), cqe);

			if (action.callback != DepLibUringPolicy::CompletionCallback::None)
			{
				settleSendCallbackNoThrow(cb, action.callback == DepLibUringPolicy::CompletionCallback::Sent);
			}
		}
	}
}

/* Static class methods */

bool DepLibUring::IsRuntimeSupported()
{
	// clang-format off
	struct utsname buffer{};
	// clang-format on

	auto err = uname(std::addressof(buffer));

	if (err != 0)
	{
		MS_THROW_ERROR("uname() failed: %s", std::strerror(errno));
	}

	MS_DEBUG_TAG(info, "kernel version: %s", buffer.version);

	auto* kernelMayorCstr = buffer.release;
	auto kernelMayorLong  = strtol(kernelMayorCstr, &kernelMayorCstr, 10);

	// liburing `sento` capabilities are supported for kernel versions greather
	// than or equal to 6.
	return kernelMayorLong >= 6;
}

void DepLibUring::ClassInit()
{
	const auto mayor = io_uring_major_version();
	const auto minor = io_uring_minor_version();

	MS_DEBUG_TAG(info, "liburing version: \"%i.%i\"", mayor, minor);

	if (DepLibUring::IsRuntimeSupported())
	{
		DepLibUring::liburing = new LibUring();

		MS_DEBUG_TAG(info, "liburing supported, enabled");
	}
	else
	{
		MS_DEBUG_TAG(info, "liburing not supported, not enabled");
	}
}

void DepLibUring::ClassDestroy()
{
	MS_TRACE();

	delete DepLibUring::liburing;
	DepLibUring::liburing = nullptr;
}

flatbuffers::Offset<FBS::LibUring::Dump> DepLibUring::FillBuffer(flatbuffers::FlatBufferBuilder& builder)
{
	MS_TRACE();

	if (!DepLibUring::liburing)
	{
		return 0;
	}

	return DepLibUring::liburing->FillBuffer(builder);
}

void DepLibUring::StartPollingCQEs()
{
	MS_TRACE();

	if (!DepLibUring::liburing)
	{
		return;
	}

	DepLibUring::liburing->StartPollingCQEs();
}

void DepLibUring::StopPollingCQEs()
{
	MS_TRACE();

	if (!DepLibUring::liburing)
	{
		return;
	}

	DepLibUring::liburing->StopPollingCQEs();
}

uint8_t* DepLibUring::GetSendBuffer()
{
	MS_TRACE();

	MS_ASSERT(DepLibUring::liburing, "DepLibUring::liburing is not set");

	return DepLibUring::liburing->GetSendBuffer();
}

bool DepLibUring::PrepareSend(
  int sockfd, const uint8_t* data, size_t len, const struct sockaddr* addr, onSendCallback* cb)
{
	MS_TRACE();

	MS_ASSERT(DepLibUring::liburing, "DepLibUring::liburing is not set");

	return DepLibUring::liburing->PrepareSend(sockfd, data, len, addr, cb);
}

bool DepLibUring::PrepareWrite(
  int sockfd, const uint8_t* data1, size_t len1, const uint8_t* data2, size_t len2, onSendCallback* cb)
{
	MS_TRACE();

	MS_ASSERT(DepLibUring::liburing, "DepLibUring::liburing is not set");

	return DepLibUring::liburing->PrepareWrite(sockfd, data1, len1, data2, len2, cb);
}

void DepLibUring::Submit()
{
	MS_TRACE();

	if (!DepLibUring::liburing)
	{
		return;
	}

	DepLibUring::liburing->Submit();
}

void DepLibUring::SetActive()
{
	MS_TRACE();

	if (!DepLibUring::liburing)
	{
		return;
	}

	DepLibUring::liburing->SetActive();
}

bool DepLibUring::IsActive()
{
	MS_TRACE();

	if (!DepLibUring::liburing)
	{
		return false;
	}

	return DepLibUring::liburing->IsActive();
}

/* Instance methods. */

DepLibUring::LibUring::LibUring()
{
	MS_TRACE();

	/**
	 * IORING_SETUP_SINGLE_ISSUER: A hint to the kernel that only a single task
	 * (or thread) will submit requests, which is used for internal optimisations.
	 */

	unsigned int flags = IORING_SETUP_SINGLE_ISSUER;

	// Initialize io_uring.
	auto err = io_uring_queue_init(DepLibUring::QueueDepth, std::addressof(this->ring), flags);

	if (err < 0)
	{
		// Get positive errno.
		int error = -err;

		MS_THROW_ERROR("io_uring_queue_init() failed: %s", std::strerror(error));
	}
	this->ringInitialized = true;

	try
	{
		// Create an eventfd instance.
		this->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

		if (this->efd < 0)
		{
			const int error = errno;

			MS_THROW_ERROR("eventfd() failed: %s", std::strerror(error));
		}

		err = io_uring_register_eventfd(std::addressof(this->ring), this->efd);

		if (err < 0)
		{
			// Get positive errno.
			int error = -err;

			MS_THROW_ERROR("io_uring_register_eventfd() failed: %s", std::strerror(error));
		}

		// Initialize available UserData entries and iovecs without allocating.
		for (size_t i{ 0 }; i < DepLibUring::QueueDepth; ++i)
		{
			this->userDatas[i].store          = this->sendBuffers[i];
			this->userDatas[i].idx            = i;
			this->availableUserDataEntries[i] = i;
			this->iovecs[i].iov_base          = this->sendBuffers[i];
			this->iovecs[i].iov_len           = DepLibUring::SendBufferSize;
		}
		this->availableUserDataEntryCount = DepLibUring::QueueDepth;

		err =
		  io_uring_register_buffers(std::addressof(this->ring), this->iovecs, DepLibUring::QueueDepth);

		if (err < 0)
		{
			// Get positive errno.
			int error = -err;

			if (error == ENOMEM)
			{
				this->zeroCopyEnabled = false;

				struct rlimit l = {};

				if (getrlimit(RLIMIT_MEMLOCK, std::addressof(l)) == -1)
				{
					MS_WARN_TAG(info, "getrlimit() failed: %s", std::strerror(errno));
					MS_WARN_TAG(
					  info,
					  "io_uring_register_buffers() failed due to low RLIMIT_MEMLOCK, disabling zero copy: %s",
					  std::strerror(error));
				}
				else
				{
					MS_WARN_TAG(
					  info,
					  "io_uring_register_buffers() failed due to low RLIMIT_MEMLOCK (soft:%lu, hard:%lu), disabling zero copy: %s",
					  l.rlim_cur,
					  l.rlim_max,
					  std::strerror(error));
				}
			}
			else
			{
				MS_THROW_ERROR("io_uring_register_buffers() failed: %s", std::strerror(error));
			}
		}
	}
	catch (...)
	{
		if (this->efd >= 0)
		{
			close(this->efd);
			this->efd = -1;
		}

		if (this->ringInitialized)
		{
			io_uring_queue_exit(std::addressof(this->ring));
			this->ringInitialized = false;
		}
		throw;
	}
}

DepLibUring::LibUring::~LibUring()
{
	MS_TRACE();

	StopPollingCQEs();

	// Close the event file descriptor.
	if (this->efd >= 0 && close(this->efd) != 0)
	{
		const int error = errno;

		MS_ERROR("close() failed: %s", std::strerror(error));
	}
	this->efd = -1;

	// Close the ring.
	if (this->ringInitialized)
	{
		io_uring_queue_exit(std::addressof(this->ring));
		this->ringInitialized = false;
	}

	// CQEs are no longer reachable after queue exit. Release callbacks still
	// owned by prepared or in-flight entries without invoking owner state during
	// global shutdown.
	for (auto& userData : this->userDatas)
	{
		delete userData.cb;
		userData.cb = nullptr;
	}
}

flatbuffers::Offset<FBS::LibUring::Dump> DepLibUring::LibUring::FillBuffer(
  flatbuffers::FlatBufferBuilder& builder) const
{
	MS_TRACE();

	return FBS::LibUring::CreateDump(
	  builder, this->sqeProcessCount, this->sqeMissCount, this->userDataMissCount);
}

void DepLibUring::LibUring::StartPollingCQEs()
{
	MS_TRACE();

	// Watch the event file descriptor for completions.
	this->uvHandle = new uv_poll_t;

	auto err = uv_poll_init(DepLibUV::GetLoop(), this->uvHandle, this->efd);

	if (err != 0)
	{
		delete this->uvHandle;
		this->uvHandle = nullptr;

		MS_THROW_ERROR("uv_poll_init() failed: %s", uv_strerror(err));
	}

	this->uvHandle->data = this;

	err = uv_poll_start(this->uvHandle, UV_READABLE, static_cast<uv_poll_cb>(onFdEvent));

	if (err != 0)
	{
		auto* uvHandle = this->uvHandle;
		this->uvHandle = nullptr;
		uvHandle->data = nullptr;
		uv_close(reinterpret_cast<uv_handle_t*>(uvHandle), static_cast<uv_close_cb>(onCloseFd));

		MS_THROW_ERROR("uv_poll_start() failed: %s", uv_strerror(err));
	}

	try
	{
		this->submitRetryHandle = new uv_timer_t;
	}
	catch (...)
	{
		StopPollingCQEs();

		throw;
	}
	err = uv_timer_init(DepLibUV::GetLoop(), this->submitRetryHandle);

	if (err != 0)
	{
		delete this->submitRetryHandle;
		this->submitRetryHandle = nullptr;
		StopPollingCQEs();

		MS_THROW_ERROR("uv_timer_init() failed: %s", uv_strerror(err));
	}
	this->submitRetryHandle->data = this;
}

void DepLibUring::LibUring::StopPollingCQEs()
{
	MS_TRACE();

	if (this->submitRetryHandle)
	{
		auto* retryHandle       = this->submitRetryHandle;
		this->submitRetryHandle = nullptr;
		retryHandle->data       = nullptr;
		(void)uv_timer_stop(retryHandle);
		uv_close(
		  reinterpret_cast<uv_handle_t*>(retryHandle), static_cast<uv_close_cb>(onCloseSubmitRetry));
	}

	if (!this->uvHandle)
	{
		return;
	}

	auto* uvHandle = this->uvHandle;
	this->uvHandle = nullptr;
	uvHandle->data = nullptr;

	// Stop polling the event file descriptor.
	auto err = uv_poll_stop(uvHandle);

	if (err != 0)
	{
		MS_ERROR("uv_poll_stop() failed: %s", uv_strerror(err));
	}

	// NOTE: Handles that wrap file descriptors are closed immediately.
	uv_close(reinterpret_cast<uv_handle_t*>(uvHandle), static_cast<uv_close_cb>(onCloseFd));
}

uint8_t* DepLibUring::LibUring::GetSendBuffer()
{
	MS_TRACE();

	if (!this->operational)
	{
		return nullptr;
	}

	auto* sendBuffer = this->PeekSendBuffer();

	if (!sendBuffer)
	{
		MS_DEBUG_DEV("no user data entry available");

		return nullptr;
	}

	return sendBuffer;
}

bool DepLibUring::LibUring::PrepareSend(
  int sockfd, const uint8_t* data, size_t len, const struct sockaddr* addr, onSendCallback* cb)
{
	MS_TRACE();
	if (!this->operational)
	{
		return false;
	}

	if (len == 0u || !DepLibUring::CanStoreSendData(len) || !data || !addr)
	{
		return false;
	}

	auto* userData = this->GetUserData();

	if (!userData)
	{
		MS_DEBUG_DEV("no user data entry available");

		this->userDataMissCount++;

		return false;
	}

	auto* sqe = io_uring_get_sqe(std::addressof(this->ring));

	if (!sqe)
	{
		MS_DEBUG_DEV("no sqe available");

		this->sqeMissCount++;
		ReleaseUserDataEntry(userData->idx);

		return false;
	}

	// The send data buffer belongs to this exact reservation, no need to memcpy.
	if (data != userData->store)
	{
		std::memcpy(userData->store, data, len);
	}

	userData->cb          = cb;
	userData->expectedLen = len;

	io_uring_sqe_set_data(sqe, userData);

	socklen_t addrlen = Utils::IP::GetAddressLen(addr);

	if (this->zeroCopyEnabled)
	{
		auto iovec    = this->iovecs[userData->idx];
		iovec.iov_len = len;

		io_uring_prep_send_zc(sqe, sockfd, iovec.iov_base, iovec.iov_len, 0, 0);
		io_uring_prep_send_set_addr(sqe, addr, addrlen);

		// Tell io_uring that we are providing the already registered send buffer
		// for zero copy.
		sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
		sqe->buf_index = userData->idx;
	}
	else
	{
		io_uring_prep_sendto(sqe, sockfd, userData->store, len, 0, addr, addrlen);
	}

	this->sqeProcessCount++;

	return true;
}

bool DepLibUring::LibUring::PrepareWrite(
  int sockfd, const uint8_t* data1, size_t len1, const uint8_t* data2, size_t len2, onSendCallback* cb)
{
	MS_TRACE();
	if (!this->operational)
	{
		return false;
	}

	const bool data2InSendBuffer = data2 && data2 == this->PeekSendBuffer();
	if (
	  (len1 == 0u && len2 == 0u) || (len1 != 0u && !data1) || (len2 != 0u && !data2) ||
	  !DepLibUring::CanStoreWriteData(len1, len2, data2InSendBuffer))
	{
		return false;
	}

	auto* userData = this->GetUserData();

	if (!userData)
	{
		MS_DEBUG_DEV("no user data entry available");

		this->userDataMissCount++;

		return false;
	}

	auto* sqe = io_uring_get_sqe(std::addressof(this->ring));

	if (!sqe)
	{
		MS_DEBUG_DEV("no sqe available");

		this->sqeMissCount++;
		ReleaseUserDataEntry(userData->idx);

		return false;
	}

	// The send data buffer belongs to us, no need to memcpy.
	// NOTE: data1 contains the TCP framing buffer and data2 the actual payload.
	if (data2InSendBuffer)
	{
		MS_ASSERT(data2 == userData->store, "send buffer does not match userData store");

		// Always memcpy the frame len as it resides in the stack memory.
		if (len1 != 0u)
		{
			std::memcpy(userData->frameLen, data1, len1);
		}

		userData->iov[0].iov_base = userData->frameLen;
		userData->iov[0].iov_len  = len1;
		userData->iov[1].iov_base = userData->store;
		userData->iov[1].iov_len  = len2;
	}
	else
	{
		if (len1 != 0u)
		{
			std::memcpy(userData->store, data1, len1);
		}
		if (len2 != 0u)
		{
			std::memcpy(userData->store + len1, data2, len2);
		}

		userData->iov[0].iov_base = userData->store;
		userData->iov[0].iov_len  = len1;
		userData->iov[1].iov_base = userData->store + len1;
		userData->iov[1].iov_len  = len2;
	}

	userData->cb          = cb;
	userData->expectedLen = len1 + len2;

	io_uring_sqe_set_data(sqe, userData);

	io_uring_prep_writev(sqe, sockfd, userData->iov, 2, 0);

	this->sqeProcessCount++;

	return true;
}

void DepLibUring::LibUring::Submit()
{
	MS_TRACE();
	if (!this->operational || !this->ringInitialized)
	{
		this->active = false;
		return;
	}

	// Unset active flag.
	this->active = false;

	const auto result = DepLibUringPolicy::DrainPendingSubmissions(
	  [this]() { return io_uring_sq_ready(std::addressof(this->ring)) != 0u; },
	  [this]() { return io_uring_submit(std::addressof(this->ring)); });

	if (result.submittedCount != 0u)
	{
		MS_DEBUG_DEV("%zu submission queue entries submitted", result.submittedCount);
		// Forward progress starts a fresh deferred retry budget for any entries that
		// remain pending after this call.
		this->deferredSubmitRetryCount = 0u;
	}

	if (result.outcome == DepLibUringPolicy::SubmitDrainOutcome::Drained)
	{
		ResetSubmitRetry();

		return;
	}

	if (result.outcome == DepLibUringPolicy::SubmitDrainOutcome::RetryDeferred)
	{
		if (!ScheduleSubmitRetry())
		{
			FailClosed("io_uring submission retry budget exhausted or could not be scheduled");
		}

		return;
	}

	const int error = result.lastResult < 0 ? -result.lastResult : EIO;
	FailClosed(std::strerror(error));
}

bool DepLibUring::LibUring::ScheduleSubmitRetry() noexcept
{
	if (!this->operational || !this->submitRetryHandle)
	{
		return false;
	}

	if (uv_is_active(reinterpret_cast<uv_handle_t*>(this->submitRetryHandle)) != 0)
	{
		return true;
	}

	if (!DepLibUringPolicy::HasDeferredSubmitRetryBudget(this->deferredSubmitRetryCount))
	{
		return false;
	}

	const auto delay = DepLibUringPolicy::DeferredSubmitRetryDelayMs(this->deferredSubmitRetryCount);
	const auto err =
	  uv_timer_start(this->submitRetryHandle, static_cast<uv_timer_cb>(onSubmitRetry), delay, 0u);

	if (err != 0)
	{
		try
		{
			MS_ERROR("uv_timer_start() failed for io_uring submission retry: %s", uv_strerror(err));
		}
		catch (...)
		{
		}

		return false;
	}

	++this->deferredSubmitRetryCount;

	return true;
}

void DepLibUring::LibUring::ResetSubmitRetry() noexcept
{
	this->deferredSubmitRetryCount = 0u;

	if (this->submitRetryHandle)
	{
		(void)uv_timer_stop(this->submitRetryHandle);
	}
}

void DepLibUring::LibUring::FailClosed(const char* reason) noexcept
{
	if (!this->operational)
	{
		return;
	}

	this->operational              = false;
	this->active                   = false;
	this->deferredSubmitRetryCount = 0u;
	try
	{
		MS_ERROR("disabling io_uring after terminal failure: %s", reason ? reason : "unknown");
	}
	catch (...)
	{
	}

	StopPollingCQEs();
	if (this->ringInitialized)
	{
		io_uring_queue_exit(std::addressof(this->ring));
		this->ringInitialized = false;
	}
	if (this->efd >= 0)
	{
		(void)close(this->efd);
		this->efd = -1;
	}

	// queue_exit makes every prepared/in-flight entry unreachable. Settle each
	// callback exactly once as failed; arbitrary owner code is contained by the
	// callback boundary and cannot prevent the remaining entries from settling.
	for (auto& userData : this->userDatas)
	{
		userData.expectedLen = 0u;
		userData.inUse       = false;
		settleSendCallbackNoThrow(userData.cb, false);
	}

	this->availableUserDataEntryHead  = 0u;
	this->availableUserDataEntryCount = DepLibUring::QueueDepth;
	for (size_t idx{ 0u }; idx < DepLibUring::QueueDepth; ++idx)
	{
		this->availableUserDataEntries[idx] = idx;
	}
}

DepLibUring::UserData* DepLibUring::LibUring::GetUserData()
{
	MS_TRACE();

	if (this->availableUserDataEntryCount == 0u)
	{
		return nullptr;
	}

	const auto idx = this->availableUserDataEntries[this->availableUserDataEntryHead];
	this->availableUserDataEntryHead =
	  (this->availableUserDataEntryHead + 1u) % DepLibUring::QueueDepth;
	--this->availableUserDataEntryCount;

	auto* userData = std::addressof(this->userDatas[idx]);
	MS_ASSERT(!userData->inUse, "user data entry reserved more than once");
	MS_ASSERT(!userData->cb, "user data entry still owns a callback when reserved");
	userData->expectedLen = 0u;
	userData->inUse       = true;

	return userData;
}

uint8_t* DepLibUring::LibUring::PeekSendBuffer() const noexcept
{
	if (this->availableUserDataEntryCount == 0u)
	{
		return nullptr;
	}

	const auto idx = this->availableUserDataEntries[this->availableUserDataEntryHead];

	return this->userDatas[idx].store;
}

void DepLibUring::LibUring::ReleaseUserDataEntry(size_t idx)
{
	MS_ASSERT(idx < DepLibUring::QueueDepth, "invalid user data entry index");
	MS_ASSERT(this->userDatas[idx].inUse, "user data entry released more than once");
	MS_ASSERT(
	  this->availableUserDataEntryCount < DepLibUring::QueueDepth,
	  "user data entry released more than once");

	this->userDatas[idx].expectedLen = 0u;
	this->userDatas[idx].inUse       = false;
	const auto tail =
	  (this->availableUserDataEntryHead + this->availableUserDataEntryCount) % DepLibUring::QueueDepth;
	this->availableUserDataEntries[tail] = idx;
	++this->availableUserDataEntryCount;
}

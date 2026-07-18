#ifndef MS_DEP_LIBURING_POLICY_HPP
#define MS_DEP_LIBURING_POLICY_HPP

#include "DepLibUringBufferLimits.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace DepLibUringPolicy
{
	enum class SubmitAction
	{
		Progress,
		RetryImmediately,
		RetryDeferred,
		FailClosed
	};

	enum class SubmitDrainOutcome
	{
		Drained,
		RetryDeferred,
		FailClosed
	};

	struct SubmitDrainResult
	{
		SubmitDrainOutcome outcome{ SubmitDrainOutcome::Drained };
		int lastResult{ 0 };
		unsigned int submitCallCount{ 0u };
		size_t submittedCount{ 0u };
	};

	constexpr unsigned int MaxImmediateInterruptedSubmits{ 3u };
	constexpr unsigned int MaxDeferredSubmitRetries{ 8u };
	constexpr uint64_t MaxDeferredSubmitRetryDelayMs{ 64u };

	constexpr SubmitAction ClassifySubmitResult(int result) noexcept
	{
		if (result > 0)
		{
			return SubmitAction::Progress;
		}
		if (result == -EINTR)
		{
			return SubmitAction::RetryImmediately;
		}
		if (result == 0 || result == -EAGAIN || result == -EBUSY)
		{
			return SubmitAction::RetryDeferred;
		}

		return SubmitAction::FailClosed;
	}

	constexpr bool HasDeferredSubmitRetryBudget(unsigned int scheduledRetryCount) noexcept
	{
		return scheduledRetryCount < MaxDeferredSubmitRetries;
	}

	constexpr uint64_t DeferredSubmitRetryDelayMs(unsigned int scheduledRetryCount) noexcept
	{
		uint64_t delay{ 1u };

		for (unsigned int idx{ 0u }; idx < scheduledRetryCount; ++idx)
		{
			if (delay >= MaxDeferredSubmitRetryDelayMs)
			{
				return MaxDeferredSubmitRetryDelayMs;
			}

			delay *= 2u;
		}

		return delay;
	}

	template<typename ReadyFn, typename SubmitFn>
	SubmitDrainResult DrainPendingSubmissions(ReadyFn&& ready, SubmitFn&& submit)
	{
		SubmitDrainResult result;
		unsigned int interruptedSubmitCount{ 0u };

		while (ready())
		{
			result.lastResult = submit();
			++result.submitCallCount;

			switch (ClassifySubmitResult(result.lastResult))
			{
				case SubmitAction::Progress:
				{
					result.submittedCount += static_cast<size_t>(result.lastResult);
					interruptedSubmitCount = 0u;

					break;
				}

				case SubmitAction::RetryImmediately:
				{
					++interruptedSubmitCount;
					if (interruptedSubmitCount < MaxImmediateInterruptedSubmits)
					{
						break;
					}

					result.outcome = SubmitDrainOutcome::RetryDeferred;

					return result;
				}

				case SubmitAction::RetryDeferred:
				{
					result.outcome = SubmitDrainOutcome::RetryDeferred;

					return result;
				}

				case SubmitAction::FailClosed:
				{
					result.outcome = SubmitDrainOutcome::FailClosed;

					return result;
				}
			}
		}

		return result;
	}

	enum class CompletionCallback
	{
		None,
		Sent,
		Failed
	};

	struct CompletionAction
	{
		bool releaseUserData{ false };
		CompletionCallback callback{ CompletionCallback::None };
	};

	constexpr CompletionAction ClassifyCompletion(
	  bool zeroCopyEnabled, bool isNotification, bool hasMore, int result, size_t expectedLen) noexcept
	{
		if (zeroCopyEnabled && isNotification)
		{
			return { true, CompletionCallback::Sent };
		}

		const bool complete = DepLibUringBufferLimits::IsCompleteIoResult(result, expectedLen);

		if (zeroCopyEnabled && hasMore)
		{
			return { false, complete ? CompletionCallback::None : CompletionCallback::Failed };
		}

		return { true, complete ? CompletionCallback::Sent : CompletionCallback::Failed };
	}

	template<typename Callback>
	Callback* TakeCompletionCallback(const CompletionAction& action, Callback*& callback) noexcept
	{
		if (action.callback == CompletionCallback::None)
		{
			return nullptr;
		}

		return std::exchange(callback, nullptr);
	}

	enum class CallbackSettlement
	{
		NoCallback,
		Invoked,
		Threw
	};

	template<typename Callback>
	CallbackSettlement SettleCallbackNoThrow(Callback*& callback, bool sent) noexcept
	{
		std::unique_ptr<Callback> ownedCallback(std::exchange(callback, nullptr));

		if (!ownedCallback)
		{
			return CallbackSettlement::NoCallback;
		}

		try
		{
			(*ownedCallback)(sent);

			return CallbackSettlement::Invoked;
		}
		catch (...)
		{
			return CallbackSettlement::Threw;
		}
	}
} // namespace DepLibUringPolicy

#endif

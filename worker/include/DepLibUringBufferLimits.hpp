#ifndef MS_DEP_LIBURING_BUFFER_LIMITS_HPP
#define MS_DEP_LIBURING_BUFFER_LIMITS_HPP

#include <cstddef>

namespace DepLibUringBufferLimits
{
	constexpr size_t QueueDepth{ 1024u * 4u };
	constexpr size_t SendBufferSize{ 1500u };
	constexpr size_t FrameLenSize{ 2u };

	constexpr bool CanStoreSendData(size_t len) noexcept
	{
		return len <= SendBufferSize;
	}

	constexpr bool CanStoreSendDataWithTrailer(size_t len, size_t trailerLen) noexcept
	{
		return trailerLen <= SendBufferSize && len <= SendBufferSize - trailerLen;
	}

	constexpr bool CanStoreWriteData(size_t len1, size_t len2, bool data2InSendBuffer) noexcept
	{
		if (data2InSendBuffer)
		{
			return len1 <= FrameLenSize && len2 <= SendBufferSize;
		}

		return len1 <= SendBufferSize && len2 <= SendBufferSize - len1;
	}

	constexpr bool IsCompleteIoResult(int result, size_t expectedLen) noexcept
	{
		return result >= 0 && static_cast<size_t>(result) == expectedLen;
	}
} // namespace DepLibUringBufferLimits

#endif

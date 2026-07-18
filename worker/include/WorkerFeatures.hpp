#ifndef MS_WORKER_FEATURES_HPP
#define MS_WORKER_FEATURES_HPP

#include <string>

namespace WorkerFeatures
{
#ifdef MS_LIBURING_SUPPORTED
	inline constexpr bool IoUringMediaSend{ true };
#else
	inline constexpr bool IoUringMediaSend{ false };
#endif

	inline std::string ToJson()
	{
		return std::string(
		         R"({"workerFeatures":{"h265Rtp":true,"h265RtpVersion":1,"ioUringMediaSend":)") +
		       (IoUringMediaSend ? "true" : "false") + "}}";
	}
} // namespace WorkerFeatures

#endif

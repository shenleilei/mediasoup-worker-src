#include "WorkerFeatures.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("worker feature dump reports compile-time io_uring media support", "[worker-features]")
{
#ifdef MS_LIBURING_SUPPORTED
	CHECK(WorkerFeatures::IoUringMediaSend);
	CHECK(
	  WorkerFeatures::ToJson() ==
	  R"({"workerFeatures":{"h265Rtp":true,"h265RtpVersion":1,"ioUringMediaSend":true}})");
#else
	CHECK_FALSE(WorkerFeatures::IoUringMediaSend);
	CHECK(
	  WorkerFeatures::ToJson() ==
	  R"({"workerFeatures":{"h265Rtp":true,"h265RtpVersion":1,"ioUringMediaSend":false}})");
#endif
}

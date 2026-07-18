#include "RTC/RtpProbationGenerator.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("probation RTP payload is initialized before transmission", "[rtp][probation][memory]")
{
	RTC::RtpProbationGenerator generator;

	auto* packet = generator.GetNextPacket(1400u);
	REQUIRE(packet);
	REQUIRE(packet->GetSize() == 1400u);
	REQUIRE(packet->GetPayloadLength() == 1368u);
	REQUIRE(packet->GetPayload());
	CHECK(
	  std::all_of(
	    packet->GetPayload(),
	    packet->GetPayload() + packet->GetPayloadLength(),
	    [](uint8_t byte) { return byte == 0u; }));

	packet = generator.GetNextPacket(32u);
	REQUIRE(packet->GetSize() == 32u);
	REQUIRE(packet->GetPayloadLength() == 0u);

	packet = generator.GetNextPacket(100u);
	REQUIRE(packet->GetPayloadLength() == 68u);
	CHECK(
	  std::all_of(
	    packet->GetPayload(),
	    packet->GetPayload() + packet->GetPayloadLength(),
	    [](uint8_t byte) { return byte == 0u; }));
}

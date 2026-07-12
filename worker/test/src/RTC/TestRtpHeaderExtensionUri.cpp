#include "RTC/RtpDictionaries.hpp"
#include <catch2/catch_test_macros.hpp>

SCENARIO("convert RTP header extension URI enums", "[rtp][rtpheaderextensionuri]")
{
	SECTION("playout-delay is accepted from FlatBuffers")
	{
		const auto type =
		  RTC::RtpHeaderExtensionUri::TypeFromFbs(FBS::RtpParameters::RtpHeaderExtensionUri::PlayoutDelay);

		REQUIRE(type == RTC::RtpHeaderExtensionUri::Type::PLAYOUT_DELAY);
		REQUIRE(RTC::RtpHeaderExtensionUri::TypeToFbs(type) ==
		        FBS::RtpParameters::RtpHeaderExtensionUri::PlayoutDelay);
	}
}

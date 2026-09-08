#include "common.hpp"
#include "RTC/TransportCongestionControlClient.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace RTC;

namespace
{
	class TestTccClientListener : public TransportCongestionControlClient::Listener
	{
	public:
		void OnTransportCongestionControlClientBitrates(
		  RTC::TransportCongestionControlClient* /*tccClient*/,
		  RTC::TransportCongestionControlClient::Bitrates& bitrates) override
		{
			this->lastBitrates     = bitrates;
			this->bitrateUpdateCount++;
		}

		void OnTransportCongestionControlClientSendRtpPacket(
		  RTC::TransportCongestionControlClient* /*tccClient*/,
		  RTC::RtpPacket* /*packet*/,
		  const webrtc::PacedPacketInfo& /*pacingInfo*/) override
		{
		}

	public:
		TransportCongestionControlClient::Bitrates lastBitrates;
		size_t bitrateUpdateCount{ 0u };
	};
} // namespace

SCENARIO("GCC send-side minimum outgoing bitrate floor", "[tcc][bwe]")
{
	SECTION("minBitrate is floored at TransportCongestionControlMinOutgoingBitrate")
	{
		TestTccClientListener listener;

		TransportCongestionControlClient client(
		  &listener, RTC::BweType::TRANSPORT_CC, /*initialAvailableBitrate*/ 1000000u,
		  /*maxOutgoingBitrate*/ 0u, /*minOutgoingBitrate*/ 0u);

		// The constructor clamps the initial available bitrate to the floor.
		REQUIRE(
		  client.testGetInitialAvailableBitrate() ==
		  std::max<uint32_t>(1000000u, TransportCongestionControlMinOutgoingBitrate));

		client.testInitializeController();

		client.SetDesiredBitrate(500000u, true);

		const auto& bitrates = client.testGetBitrates();

		// Core assertion: the value handed to GoogCC as min_data_rate never
		// drops below the global floor, regardless of desired/available.
		REQUIRE(bitrates.minBitrate == TransportCongestionControlMinOutgoingBitrate);
		REQUIRE(bitrates.minBitrate == 2400000u);
		REQUIRE(bitrates.startBitrate >= TransportCongestionControlMinOutgoingBitrate);

		// A lower per-transport min request cannot undercut the global floor.
		client.SetMinOutgoingBitrate(300000u);
		REQUIRE(client.testGetBitrates().minBitrate == TransportCongestionControlMinOutgoingBitrate);

		// A higher per-transport min still wins over the floor.
		client.SetMinOutgoingBitrate(3000000u);
		REQUIRE(client.testGetBitrates().minBitrate == 3000000u);
	}
}

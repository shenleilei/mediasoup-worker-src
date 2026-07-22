#include "modules/congestion_controller/rtp/send_time_history.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"

#include <catch2/catch_test_macros.hpp>

SCENARIO("SendTimeHistory keeps duplicate packet accounting idempotent", "[webrtc][send-time-history]")
{
	webrtc::SendTimeHistory history(/*packet_age_limit_ms*/ 1000);

	webrtc::PacketFeedback packet(
	  /*creation_time_ms*/ 1,
	  /*arrival_time_ms*/ webrtc::PacketFeedback::kNotReceived,
	  /*send_time_ms*/ 10,
	  /*sequence_number*/ 1234,
	  /*payload_size*/ 1200,
	  /*local_net_id*/ 11,
	  /*remote_net_id*/ 22,
	  webrtc::PacedPacketInfo());

	history.AddNewPacket(packet);
	history.AddNewPacket(packet);

	auto stored = history.GetPacket(packet.sequence_number);
	REQUIRE(stored.has_value());
	REQUIRE(stored->sequence_number == packet.sequence_number);
	REQUIRE(stored->payload_size == packet.payload_size);
	REQUIRE(history.GetOutstandingData(packet.local_net_id, packet.remote_net_id).bytes() == 1200);

	webrtc::PacketFeedback feedback(
	  /*arrival_time_ms*/ 20,
	  /*sequence_number*/ packet.sequence_number);
	REQUIRE(history.GetFeedback(&feedback, /*remove*/ true));
	REQUIRE(feedback.payload_size == packet.payload_size);
	REQUIRE(history.GetOutstandingData(packet.local_net_id, packet.remote_net_id).bytes() == 0);
}

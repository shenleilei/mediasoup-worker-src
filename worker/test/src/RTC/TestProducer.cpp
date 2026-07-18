#include "common.hpp"
#include "ChannelMessageRegistrator.hpp"
#include "FBS/transport.h"
#include "RTC/Producer.hpp"
#include "RTC/RTCP/FeedbackPsPli.hpp"
#include "RTC/RTCP/FeedbackRtpNack.hpp"
#include "RTC/Shared.hpp"
#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace
{
	constexpr uint8_t PayloadType{ 111u };
	constexpr uint32_t ProducerSsrc{ 11111111u };
	constexpr uint32_t MappedSsrc{ 22222222u };

	const FBS::Transport::ProduceRequest* BuildProduceRequest(flatbuffers::FlatBufferBuilder& builder)
	{
		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> codecParameters;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> rtcpFeedback;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpHeaderExtensionParameters>> headerExtensions;

		auto codec = FBS::RtpParameters::CreateRtpCodecParametersDirect(
		  builder, "video/VP8", PayloadType, 90000u, flatbuffers::nullopt, &codecParameters, &rtcpFeedback);
		auto encoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(ProducerSsrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType));

		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpCodecParameters>> codecs{ codec };
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> encodings{ encoding };
		auto rtcp          = FBS::RtpParameters::CreateRtcpParametersDirect(builder, "producer-cname");
		auto rtpParameters = FBS::RtpParameters::CreateRtpParametersDirect(
		  builder, "video", &codecs, &headerExtensions, &encodings, rtcp);

		auto codecMapping = FBS::RtpParameters::CreateCodecMapping(builder, PayloadType, PayloadType);
		auto encodingMapping = FBS::RtpParameters::CreateEncodingMappingDirect(
		  builder, nullptr, flatbuffers::Optional<uint32_t>(ProducerSsrc), nullptr, MappedSsrc);
		std::vector<flatbuffers::Offset<FBS::RtpParameters::CodecMapping>> codecMappings{ codecMapping };
		std::vector<flatbuffers::Offset<FBS::RtpParameters::EncodingMapping>> encodingMappings{
			encodingMapping
		};
		auto rtpMapping =
		  FBS::RtpParameters::CreateRtpMappingDirect(builder, &codecMappings, &encodingMappings);

		auto request = FBS::Transport::CreateProduceRequestDirect(
		  builder, "producer-rtcp-feedback", FBS::RtpParameters::MediaKind::VIDEO, rtpParameters, rtpMapping);

		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Transport::ProduceRequest>(builder.GetBufferPointer());
	}

	class TestProducerListener : public RTC::Producer::Listener
	{
	public:
		void OnProducerReceiveData(RTC::Producer* /*producer*/, size_t /*len*/) override
		{
		}

		void OnProducerReceiveRtpPacket(RTC::Producer* /*producer*/, RTC::RtpPacket* /*packet*/) override
		{
		}

		void OnProducerPaused(RTC::Producer* /*producer*/) override
		{
		}

		void OnProducerResumed(RTC::Producer* /*producer*/) override
		{
		}

		void OnProducerNewRtpStream(
		  RTC::Producer* /*producer*/, RTC::RtpStreamRecv* /*rtpStream*/, uint32_t /*mappedSsrc*/) override
		{
		}

		void OnProducerRtpStreamScore(
		  RTC::Producer* /*producer*/,
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  uint8_t /*score*/,
		  uint8_t /*previousScore*/) override
		{
		}

		void OnProducerRtcpSenderReport(
		  RTC::Producer* /*producer*/, RTC::RtpStreamRecv* /*rtpStream*/, bool /*first*/) override
		{
		}

		void OnProducerRtpPacketReceived(RTC::Producer* /*producer*/, RTC::RtpPacket* /*packet*/) override
		{
		}

		void OnProducerSendRtcpPacket(RTC::Producer* /*producer*/, RTC::RTCP::Packet* packet) override
		{
			this->sentRtcpPackets.push_back(packet);
		}

		void OnProducerNeedWorstRemoteFractionLost(
		  RTC::Producer* /*producer*/, uint32_t /*mappedSsrc*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

	public:
		std::vector<RTC::RTCP::Packet*> sentRtcpPackets;
	};
} // namespace

TEST_CASE(
  "Producer forwards PLI and NACK without crossing RTCP feedback families",
  "[producer][rtcp][feedback]")
{
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(builder);
	RTC::Producer producer(&shared, "producer-rtcp-feedback", &listener, request);

	// Regression: PSFB used to fall through into RTPFB and downcast this PLI to a sibling type.
	RTC::RTCP::FeedbackPsPliPacket pli(/*senderSsrc*/ 33333333u, ProducerSsrc);

	producer.OnRtpStreamSendRtcpPacket(nullptr, &pli);

	REQUIRE(listener.sentRtcpPackets.size() == 1u);
	CHECK(listener.sentRtcpPackets[0] == &pli);
	REQUIRE(listener.sentRtcpPackets[0]->GetType() == RTC::RTCP::Type::PSFB);
	auto* forwardedPli = dynamic_cast<RTC::RTCP::FeedbackPsPacket*>(listener.sentRtcpPackets[0]);
	REQUIRE(forwardedPli != nullptr);
	CHECK(forwardedPli->GetMessageType() == RTC::RTCP::FeedbackPs::MessageType::PLI);

	RTC::RTCP::FeedbackRtpNackPacket nack(/*senderSsrc*/ 44444444u, ProducerSsrc);

	producer.OnRtpStreamSendRtcpPacket(nullptr, &nack);

	REQUIRE(listener.sentRtcpPackets.size() == 2u);
	CHECK(listener.sentRtcpPackets[1] == &nack);
	REQUIRE(listener.sentRtcpPackets[1]->GetType() == RTC::RTCP::Type::RTPFB);
	auto* forwardedNack = dynamic_cast<RTC::RTCP::FeedbackRtpPacket*>(listener.sentRtcpPackets[1]);
	REQUIRE(forwardedNack != nullptr);
	CHECK(forwardedNack->GetMessageType() == RTC::RTCP::FeedbackRtp::MessageType::NACK);
}

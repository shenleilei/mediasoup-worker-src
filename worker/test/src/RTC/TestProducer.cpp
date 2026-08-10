#include "common.hpp"
#include "ChannelMessageRegistrator.hpp"
#include "Utils.hpp"
#include "Channel/ChannelNotifier.hpp"
#include "Channel/ChannelSocket.hpp"
#include "FBS/transport.h"
#include "RTC/Producer.hpp"
#include "RTC/RTCP/FeedbackPsPli.hpp"
#include "RTC/RTCP/FeedbackRtpNack.hpp"
#include "RTC/RTCP/SenderReport.hpp"
#include "RTC/RtpPacket.hpp"
#include "RTC/Shared.hpp"
#include <flatbuffers/flatbuffers.h>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

namespace
{
	constexpr uint8_t PayloadType{ 111u };
	constexpr uint32_t ProducerSsrc{ 11111111u };
	constexpr uint32_t MappedSsrc{ 22222222u };

	ChannelReadFreeFn NoChannelMessage(
	  uint8_t** /*message*/,
	  uint32_t* /*messageLen*/,
	  size_t* /*messageCtx*/,
	  const void* /*handle*/,
	  ChannelReadCtx /*ctx*/)
	{
		return nullptr;
	}

	void IgnoreChannelWrite(const uint8_t* /*message*/, uint32_t /*messageLen*/, ChannelWriteCtx /*ctx*/)
	{
	}

	const FBS::Transport::ProduceRequest* BuildProduceRequest(flatbuffers::FlatBufferBuilder& builder)
	{
		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> codecParameters;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> rtcpFeedback;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpHeaderExtensionParameters>> headerExtensions;
		headerExtensions.emplace_back(
		  FBS::RtpParameters::CreateRtpHeaderExtensionParametersDirect(
		    builder, FBS::RtpParameters::RtpHeaderExtensionUri::AbsCaptureTime, 9u));

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

		void OnProducerSendRtcpPacket(RTC::Producer* /*producer*/, RTC::RTCP::Packet* packet) override
		{
			this->sentRtcpPackets.push_back(packet);
		}

		void OnProducerNeedWorstRemoteFractionLost(
		  RTC::Producer* /*producer*/, uint32_t /*mappedSsrc*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnProducerRtpPacketReceived(RTC::Producer* /*producer*/, RTC::RtpPacket* packet) override
		{
			RtpPacketSnapshot snapshot;
			snapshot.ssrc = packet->GetSsrc();
			uint8_t length{ 0u };
			if (
			  auto* value = packet->GetExtension(
			    static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_CAPTURE_TIME), length))
			{
				snapshot.absCaptureTime.assign(value, value + length);
			}
			this->receivedRtpPackets.emplace_back(std::move(snapshot));
		}

	public:
		std::vector<RTC::RTCP::Packet*> sentRtcpPackets;

		struct RtpPacketSnapshot
		{
			uint32_t ssrc{ 0u };
			std::vector<uint8_t> absCaptureTime;
		};
		std::vector<RtpPacketSnapshot> receivedRtpPackets;
	};

	struct AbsCapturePacket
	{
		AbsCapturePacket(
		  uint16_t sequenceNumber, uint32_t timestamp, const std::array<uint8_t, 16u>& absCaptureTime)
		{
			this->buffer[0]  = 0x80u;
			this->buffer[1]  = PayloadType;
			this->buffer[12] = 0x00u; // VP8 key frame payload descriptor.
			this->buffer[13] = 0x00u;
			this->packet.reset(
			  RTC::RtpPacket::Parse(
			    this->buffer.data(), RTC::RtpPacket::HeaderSize + 2u, this->buffer.size()));
			REQUIRE(this->packet);
			this->packet->SetPayloadType(PayloadType);
			this->packet->SetSequenceNumber(sequenceNumber);
			this->packet->SetTimestamp(timestamp);
			this->packet->SetSsrc(ProducerSsrc);
			this->packet->SetMarker(true);
			auto absCaptureBytes = absCaptureTime;
			std::vector<RTC::RtpPacket::GenericExtension> extensions{
				{ 9u, static_cast<uint8_t>(absCaptureTime.size()), absCaptureBytes.data() }
			};
			REQUIRE(this->packet->SetExtensions(1u, extensions));
			this->packet->SetAbsCaptureTimeExtensionId(9u);
		}

		std::array<uint8_t, 1600u> buffer{};
		std::unique_ptr<RTC::RtpPacket> packet;
	};
} // namespace

TEST_CASE(
  "Producer forwards PLI and NACK without crossing RTCP feedback families",
  "[producer][rtcp][feedback]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
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

TEST_CASE(
  "Producer rewrites absolute capture time after an RTCP sender report",
  "[producer][rtp][abs-capture-time]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(builder);
	RTC::Producer producer(&shared, "producer-abs-capture", &listener, request);

	const std::array<uint8_t, 16u> inputAbsCaptureTime{ 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u,
		                                                  0x07u, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u,
		                                                  0x00u, 0x00u, 0x00u, 0x00u };

	// Before an RTCP clock sample is available, the proxy must strip the
	// untrusted offset and forward the short 8-byte form.
	AbsCapturePacket firstPacket(1u, 90000u, inputAbsCaptureTime);
	CHECK(
	  producer.ReceiveRtpPacket(firstPacket.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.receivedRtpPackets.size() == 1u);
	CHECK(listener.receivedRtpPackets[0].ssrc == MappedSsrc);
	REQUIRE(listener.receivedRtpPackets[0].absCaptureTime.size() == 8u);

	const auto localWallClockMs = Utils::Time::GetRealTimeMs();
	const auto senderNtp64      = Utils::Time::UnixMsToNtp64(localWallClockMs + 500u);
	RTC::RTCP::SenderReport senderReport;
	senderReport.SetSsrc(ProducerSsrc);
	senderReport.SetNtpSec(static_cast<uint32_t>(senderNtp64 >> 32u));
	senderReport.SetNtpFrac(static_cast<uint32_t>(senderNtp64 & 0xffffffffULL));
	senderReport.SetRtpTs(90000u);
	producer.ReceiveRtcpSenderReport(&senderReport);

	// Once the SR-derived sender/SFU offset is available, the proxy must keep
	// the 16-byte form and add that offset to the capture clock offset field.
	AbsCapturePacket secondPacket(2u, 93000u, inputAbsCaptureTime);
	CHECK(
	  producer.ReceiveRtpPacket(secondPacket.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.receivedRtpPackets.size() == 2u);
	const auto& forwarded = listener.receivedRtpPackets[1].absCaptureTime;
	REQUIRE(forwarded.size() == 16u);
	CHECK(std::equal(inputAbsCaptureTime.begin(), inputAbsCaptureTime.begin() + 8, forwarded.begin()));
	const auto forwardedOffsetMs =
	  Utils::Time::SignedNtp64ToMs(static_cast<int64_t>(Utils::Byte::Get8Bytes(forwarded.data(), 8u)));
	CHECK(forwardedOffsetMs >= 300);
	CHECK(forwardedOffsetMs <= 700);
}

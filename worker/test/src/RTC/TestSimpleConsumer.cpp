#include "common.hpp"
#include "ChannelMessageRegistrator.hpp"
#include "FBS/transport.h"
#include "RTC/Codecs/H264.hpp"
#include "RTC/Codecs/H265.hpp"
#include "RTC/RtpStreamRecv.hpp"
#include "RTC/Shared.hpp"
#include "RTC/SimpleConsumer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <flatbuffers/flatbuffers.h>
#include <memory>
#include <vector>

using namespace RTC;

namespace
{
	constexpr uint8_t PayloadType{ 111 };
	constexpr uint32_t ProducerSsrc{ 11111111u };
	constexpr uint32_t ConsumerSsrc{ 22222222u };

	struct MediaFixture
	{
		const char* mimeType{ nullptr };
		uint8_t (*nalType)(const uint8_t* payload){ nullptr };
		void (*processRtpPacket)(RTC::RtpPacket* packet){ nullptr };
	};

	uint8_t H265HeaderByte(uint8_t nalType)
	{
		return static_cast<uint8_t>((nalType & 0x3F) << 1);
	}

	uint8_t H264NalType(const uint8_t* payload)
	{
		return payload[0] & 0x1F;
	}

	uint8_t H265NalType(const uint8_t* payload)
	{
		return static_cast<uint8_t>((payload[0] >> 1) & 0x3F);
	}

	void ProcessH264RtpPacket(RTC::RtpPacket* packet)
	{
		RTC::Codecs::H264::ProcessRtpPacket(packet);
	}

	void ProcessH265RtpPacket(RTC::RtpPacket* packet)
	{
		RTC::Codecs::H265::ProcessRtpPacket(packet);
	}

	const MediaFixture H264Fixture{ "video/H264", H264NalType, ProcessH264RtpPacket };
	const MediaFixture H265Fixture{ "video/H265", H265NalType, ProcessH265RtpPacket };

	std::vector<uint8_t> H264NalUnit(uint8_t nalType)
	{
		return { nalType, 0xaa };
	}

	void AppendH264NalUnit(std::vector<uint8_t>& payload, const std::vector<uint8_t>& nalu)
	{
		payload.push_back(static_cast<uint8_t>((nalu.size() >> 8) & 0xFF));
		payload.push_back(static_cast<uint8_t>(nalu.size() & 0xFF));
		payload.insert(payload.end(), nalu.begin(), nalu.end());
	}

	std::vector<uint8_t> H264StapAPacket(std::vector<uint8_t> nalTypes)
	{
		std::vector<uint8_t> payload{ 24u };

		for (auto nalType : nalTypes)
		{
			AppendH264NalUnit(payload, H264NalUnit(nalType));
		}

		return payload;
	}

	std::vector<uint8_t> H265NalUnit(uint8_t nalType)
	{
		return { H265HeaderByte(nalType), 0x01, 0xaa };
	}

	void AppendH265NalUnit(std::vector<uint8_t>& payload, const std::vector<uint8_t>& nalu)
	{
		payload.push_back(static_cast<uint8_t>((nalu.size() >> 8) & 0xFF));
		payload.push_back(static_cast<uint8_t>(nalu.size() & 0xFF));
		payload.insert(payload.end(), nalu.begin(), nalu.end());
	}

	std::vector<uint8_t> H265AggregationPacket(std::vector<uint8_t> nalTypes)
	{
		std::vector<uint8_t> payload{ H265HeaderByte(48u), 0x01 };

		for (auto nalType : nalTypes)
		{
			AppendH265NalUnit(payload, H265NalUnit(nalType));
		}

		return payload;
	}

	std::vector<uint8_t> RtpPacketBuffer(std::vector<uint8_t> payload)
	{
		std::vector<uint8_t> buffer{
			0x80, PayloadType, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00,        0x00, 0x00, 0x00, 0x00
		};

		buffer.insert(buffer.end(), payload.begin(), payload.end());

		return buffer;
	}

	const FBS::Transport::ConsumeRequest* BuildConsumeRequest(
	  flatbuffers::FlatBufferBuilder& builder, const MediaFixture& fixture)
	{
		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> codecParameters;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> rtcpFeedback;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpHeaderExtensionParameters>> headerExtensions;

		auto codec = FBS::RtpParameters::CreateRtpCodecParametersDirect(
		  builder,
		  fixture.mimeType,
		  PayloadType,
		  90000u,
		  flatbuffers::nullopt,
		  &codecParameters,
		  &rtcpFeedback);

		auto consumerEncoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(ConsumerSsrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType));

		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpCodecParameters>> codecs{ codec };
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> encodings{
			consumerEncoding
		};

		auto rtcp = FBS::RtpParameters::CreateRtcpParametersDirect(builder, "consumer-cname");
		auto rtpParameters = FBS::RtpParameters::CreateRtpParametersDirect(
		  builder, "video", &codecs, &headerExtensions, &encodings, rtcp);

		auto consumableEncoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(ProducerSsrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType));
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>>
		  consumableRtpEncodings{ consumableEncoding };

		auto request = FBS::Transport::CreateConsumeRequestDirect(
		  builder,
		  "consumer-h265-sync",
		  "producer-h265-sync",
		  FBS::RtpParameters::MediaKind::VIDEO,
		  rtpParameters,
		  FBS::RtpParameters::Type::SIMPLE,
		  &consumableRtpEncodings);

		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Transport::ConsumeRequest>(builder.GetBufferPointer());
	}

	class TestConsumerListener : public RTC::Consumer::Listener
	{
	public:
		explicit TestConsumerListener(const MediaFixture& fixture) : fixture(fixture)
		{
		}

		void OnConsumerSendRtpPacket(RTC::Consumer* /*consumer*/, RTC::RtpPacket* packet) override
		{
			this->sentNalTypes.push_back(this->fixture.nalType(packet->GetPayload()));
		}

		void OnConsumerRetransmitRtpPacket(RTC::Consumer* /*consumer*/, RTC::RtpPacket* /*packet*/) override
		{
		}

		void OnConsumerKeyFrameRequested(RTC::Consumer* /*consumer*/, uint32_t /*mappedSsrc*/) override
		{
			++this->keyFrameRequests;
		}

		void OnConsumerNeedBitrateChange(RTC::Consumer* /*consumer*/) override
		{
		}

		void OnConsumerNeedZeroBitrate(RTC::Consumer* /*consumer*/) override
		{
		}

		void OnConsumerProducerClosed(RTC::Consumer* /*consumer*/) override
		{
		}

	public:
		const MediaFixture& fixture;
		std::vector<uint8_t> sentNalTypes;
		size_t keyFrameRequests{ 0u };
	};

	class TestRtpStreamRecvListener : public RTC::RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(
		  RTC::RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
		}

		void OnRtpStreamSendRtcpPacket(
		  RTC::RtpStreamRecv* /*rtpStream*/, RTC::RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}
	};

	void SendPayload(
	  const MediaFixture& fixture,
	  RTC::SimpleConsumer& consumer,
	  uint16_t seq,
	  uint32_t timestamp,
	  std::vector<uint8_t> payload)
	{
		auto buffer = RtpPacketBuffer(std::move(payload));
		std::unique_ptr<RTC::RtpPacket> packet(RTC::RtpPacket::Parse(buffer.data(), buffer.size()));

		REQUIRE(packet);

		packet->SetPayloadType(PayloadType);
		packet->SetSequenceNumber(seq);
		packet->SetTimestamp(timestamp);
		packet->SetSsrc(ProducerSsrc);
		packet->SetMarker(true);

		fixture.processRtpPacket(packet.get());

		std::shared_ptr<RTC::RtpPacket> sharedPacket;
		consumer.SendRtpPacket(packet.get(), sharedPacket);
	}

	void SendH265NalUnit(RTC::SimpleConsumer& consumer, uint16_t seq, uint32_t timestamp, uint8_t nalType)
	{
		SendPayload(H265Fixture, consumer, seq, timestamp, H265NalUnit(nalType));
	}

	void SendH264NalUnit(RTC::SimpleConsumer& consumer, uint16_t seq, uint32_t timestamp, uint8_t nalType)
	{
		SendPayload(H264Fixture, consumer, seq, timestamp, H264NalUnit(nalType));
	}

	void SetupActiveSyncConsumer(RTC::SimpleConsumer& consumer, RTC::RtpStreamRecv& producerStream)
	{
		consumer.TransportConnected();
		consumer.ProducerRtpStream(&producerStream, ProducerSsrc);
	}
}

SCENARIO("SimpleConsumer forwards H265 parameter sets while waiting for sync", "[consumer][h265]")
{
	ChannelMessageRegistrator* registrator = new ChannelMessageRegistrator();
	RTC::Shared shared(registrator, nullptr);
	TestConsumerListener consumerListener(H265Fixture);

	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H265Fixture);
	RTC::SimpleConsumer consumer(&shared, "consumer-h265-sync", "producer-h265-sync", &consumerListener, request);

	TestRtpStreamRecvListener rtpStreamRecvListener;
	RTC::RtpStream::Params producerParams;

	producerParams.ssrc        = ProducerSsrc;
	producerParams.payloadType = PayloadType;
	producerParams.clockRate   = 90000u;
	producerParams.mimeType.SetMimeType(H265Fixture.mimeType);

	RTC::RtpStreamRecv producerStream(
	  &rtpStreamRecvListener,
	  producerParams,
	  /*sendNackDelayMs*/ 0u,
	  /*useRtpInactivityCheck*/ false);

	SetupActiveSyncConsumer(consumer, producerStream);

	SendH265NalUnit(consumer, 1000u, 90000u, 32u); // VPS.
	SendH265NalUnit(consumer, 1001u, 90000u, 33u); // SPS.
	SendH265NalUnit(consumer, 1002u, 90000u, 34u); // PPS.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 32u, 33u, 34u });

	SendH265NalUnit(consumer, 1003u, 90000u, 0u); // Non-key slice before IRAP.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 32u, 33u, 34u });

	SendH265NalUnit(consumer, 1004u, 90000u, 19u); // IRAP.
	SendH265NalUnit(consumer, 1005u, 93000u, 0u);  // Normal non-key slice after sync.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 32u, 33u, 34u, 19u, 0u });
}

SCENARIO("SimpleConsumer forwards H265 AP parameter sets while waiting for sync", "[consumer][h265]")
{
	ChannelMessageRegistrator* registrator = new ChannelMessageRegistrator();
	RTC::Shared shared(registrator, nullptr);
	TestConsumerListener consumerListener(H265Fixture);

	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H265Fixture);
	RTC::SimpleConsumer consumer(
	  &shared, "consumer-h265-sync-ap", "producer-h265-sync", &consumerListener, request);

	TestRtpStreamRecvListener rtpStreamRecvListener;
	RTC::RtpStream::Params producerParams;

	producerParams.ssrc        = ProducerSsrc;
	producerParams.payloadType = PayloadType;
	producerParams.clockRate   = 90000u;
	producerParams.mimeType.SetMimeType(H265Fixture.mimeType);

	RTC::RtpStreamRecv producerStream(
	  &rtpStreamRecvListener,
	  producerParams,
	  /*sendNackDelayMs*/ 0u,
	  /*useRtpInactivityCheck*/ false);

	SetupActiveSyncConsumer(consumer, producerStream);

	SendPayload(H265Fixture, consumer, 2000u, 90000u, H265AggregationPacket({ 32u, 33u, 34u }));

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 48u });

	std::vector<uint8_t> malformedAp{ H265HeaderByte(48u), 0x01, 0x00, 0x01, H265HeaderByte(32u) };

	SendPayload(H265Fixture, consumer, 2001u, 90000u, malformedAp);

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 48u });

	SendH265NalUnit(consumer, 2002u, 90000u, 19u); // IRAP.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 48u, 19u });
}

SCENARIO("SimpleConsumer preserves H264 parameter-set sync behavior", "[consumer][h264]")
{
	ChannelMessageRegistrator* registrator = new ChannelMessageRegistrator();
	RTC::Shared shared(registrator, nullptr);
	TestConsumerListener consumerListener(H264Fixture);

	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H264Fixture);
	RTC::SimpleConsumer consumer(
	  &shared, "consumer-h264-sync", "producer-h264-sync", &consumerListener, request);

	TestRtpStreamRecvListener rtpStreamRecvListener;
	RTC::RtpStream::Params producerParams;

	producerParams.ssrc        = ProducerSsrc;
	producerParams.payloadType = PayloadType;
	producerParams.clockRate   = 90000u;
	producerParams.mimeType.SetMimeType(H264Fixture.mimeType);

	RTC::RtpStreamRecv producerStream(
	  &rtpStreamRecvListener,
	  producerParams,
	  /*sendNackDelayMs*/ 0u,
	  /*useRtpInactivityCheck*/ false);

	SetupActiveSyncConsumer(consumer, producerStream);

	SendH264NalUnit(consumer, 3000u, 90000u, 7u); // SPS.
	SendH264NalUnit(consumer, 3001u, 90000u, 8u); // PPS.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 7u, 8u });

	SendH264NalUnit(consumer, 3002u, 90000u, 1u); // Non-IDR slice before IDR.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 7u, 8u });

	SendH264NalUnit(consumer, 3003u, 90000u, 5u); // IDR.
	SendH264NalUnit(consumer, 3004u, 93000u, 1u); // Normal non-IDR slice after sync.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 7u, 8u, 5u, 1u });
}

SCENARIO("SimpleConsumer preserves H264 STAP-A sync behavior", "[consumer][h264]")
{
	ChannelMessageRegistrator* registrator = new ChannelMessageRegistrator();
	RTC::Shared shared(registrator, nullptr);
	TestConsumerListener consumerListener(H264Fixture);

	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H264Fixture);
	RTC::SimpleConsumer consumer(
	  &shared, "consumer-h264-stap-a-sync", "producer-h264-sync", &consumerListener, request);

	TestRtpStreamRecvListener rtpStreamRecvListener;
	RTC::RtpStream::Params producerParams;

	producerParams.ssrc        = ProducerSsrc;
	producerParams.payloadType = PayloadType;
	producerParams.clockRate   = 90000u;
	producerParams.mimeType.SetMimeType(H264Fixture.mimeType);

	RTC::RtpStreamRecv producerStream(
	  &rtpStreamRecvListener,
	  producerParams,
	  /*sendNackDelayMs*/ 0u,
	  /*useRtpInactivityCheck*/ false);

	SetupActiveSyncConsumer(consumer, producerStream);

	SendPayload(H264Fixture, consumer, 4000u, 90000u, H264StapAPacket({ 7u, 8u }));

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 24u });

	SendH264NalUnit(consumer, 4001u, 90000u, 1u); // Non-IDR slice before IDR.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 24u });

	SendH264NalUnit(consumer, 4002u, 90000u, 5u); // IDR.
	SendH264NalUnit(consumer, 4003u, 93000u, 1u); // Normal non-IDR slice after sync.

	REQUIRE(consumerListener.sentNalTypes == std::vector<uint8_t>{ 24u, 5u, 1u });
}

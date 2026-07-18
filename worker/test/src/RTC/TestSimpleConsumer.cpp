#include "common.hpp"
#include "ChannelMessageRegistrator.hpp"
#include "MediaSoupErrors.hpp"
#include "FBS/transport.h"
#include "RTC/Codecs/H264.hpp"
#include "RTC/Codecs/H264_SVC.hpp"
#include "RTC/Codecs/H265.hpp"
#include "RTC/Codecs/Opus.hpp"
#include "RTC/Codecs/VP8.hpp"
#include "RTC/PipeConsumer.hpp"
#include "RTC/Producer.hpp"
#include "RTC/Router.hpp"
#include "RTC/RtpStreamRecv.hpp"
#include "RTC/Shared.hpp"
#include "RTC/SimpleConsumer.hpp"
#include "RTC/SimulcastConsumer.hpp"
#include "RTC/SvcConsumer.hpp"
#include <flatbuffers/flatbuffers.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
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

	void ProcessH264SvcRtpPacket(RTC::RtpPacket* packet)
	{
		RTC::Codecs::H264_SVC::ProcessRtpPacket(packet);
	}

	void ProcessH265RtpPacket(RTC::RtpPacket* packet)
	{
		RTC::Codecs::H265::ProcessRtpPacket(packet);
	}

	uint8_t OpusPayloadType(const uint8_t* payload)
	{
		return payload[0];
	}

	void ProcessOpusRtpPacket(RTC::RtpPacket* packet)
	{
		RTC::Codecs::Opus::ProcessRtpPacket(packet);
	}

	void ProcessVP8RtpPacket(RTC::RtpPacket* packet)
	{
		if (!RTC::Codecs::VP8::ProcessRtpPacket(packet))
		{
			throw std::runtime_error("failed to normalize VP8 RTP packet");
		}
	}

	const MediaFixture H264Fixture{ "video/H264", H264NalType, ProcessH264RtpPacket };
	const MediaFixture H264SvcFixture{
	  "video/H264-SVC", H264NalType, ProcessH264SvcRtpPacket
	};
	const MediaFixture H265Fixture{ "video/H265", H265NalType, ProcessH265RtpPacket };
	const MediaFixture OpusFixture{ "audio/opus", OpusPayloadType, ProcessOpusRtpPacket };
	const MediaFixture VP8Fixture{ "video/VP8", OpusPayloadType, ProcessVP8RtpPacket };

	ChannelReadFreeFn NoChannelMessage(
	  uint8_t** /*message*/,
	  uint32_t* /*messageLen*/,
	  size_t* /*messageCtx*/,
	  const void* /*handle*/,
	  ChannelReadCtx /*ctx*/)
	{
		return nullptr;
	}

	void IgnoreChannelWrite(
	  const uint8_t* /*message*/, uint32_t /*messageLen*/, ChannelWriteCtx /*ctx*/)
	{
	}

	struct ConsumerConfig
	{
		const char* consumerId{ "consumer-h265-sync" };
		const char* producerId{ "producer-h265-sync" };
		const char* mid{ "video" };
		uint32_t ssrc{ ConsumerSsrc };
		uint8_t midId{ 0u };
		uint8_t absSendTimeId{ 0u };
		uint8_t transportWideCcId{ 0u };
		bool useNack{ false };
		const char* scalabilityMode{ nullptr };
	};

	struct ExtensionProfile
	{
		uint8_t midId{ 0u };
		uint8_t absSendTimeId{ 0u };
		uint8_t transportWideCcId{ 0u };
	};

	struct PacketSnapshot
	{
		const uint8_t* data{ nullptr };
		uint32_t ssrc{ 0u };
		uint16_t sequenceNumber{ 0u };
		uint32_t timestamp{ 0u };
		bool marker{ false };
		std::string mid;
		uint32_t absSendTime{ 0u };
		uint16_t transportWideCc{ 0u };
		bool hasAbsSendTime{ false };
		bool hasTransportWideCc{ false };
		bool hasOneByteExtensions{ false };
		bool hasTwoBytesExtensions{ false };
		std::vector<uint8_t> extensionIds;
		std::vector<uint8_t> payload;
		std::vector<uint8_t> bytes;
	};

	PacketSnapshot CapturePacket(RTC::RtpPacket* packet, const ExtensionProfile& profile)
	{
		PacketSnapshot snapshot;

		snapshot.data                  = packet->GetData();
		snapshot.ssrc                  = packet->GetSsrc();
		snapshot.sequenceNumber        = packet->GetSequenceNumber();
		snapshot.timestamp             = packet->GetTimestamp();
		snapshot.marker                = packet->HasMarker();
		snapshot.hasOneByteExtensions  = packet->HasOneByteExtensions();
		snapshot.hasTwoBytesExtensions = packet->HasTwoBytesExtensions();
		if (packet->GetPayloadLength() != 0u)
		{
			snapshot.payload.assign(
			  packet->GetPayload(), packet->GetPayload() + packet->GetPayloadLength());
		}
		snapshot.bytes.assign(packet->GetData(), packet->GetData() + packet->GetSize());

		for (uint16_t id{ 1u }; id <= 255u; ++id)
		{
			if (packet->HasExtension(static_cast<uint8_t>(id)))
			{
				snapshot.extensionIds.push_back(static_cast<uint8_t>(id));
			}
		}

		if (profile.midId != 0u)
		{
			uint8_t length{ 0u };
			auto* value = packet->GetExtension(profile.midId, length);

			if (value)
			{
				snapshot.mid.assign(reinterpret_cast<const char*>(value), length);
			}
		}
		if (profile.absSendTimeId != 0u)
		{
			uint8_t length{ 0u };
			auto* value = packet->GetExtension(profile.absSendTimeId, length);
			if (value && length == 3u)
			{
				snapshot.hasAbsSendTime = true;
				snapshot.absSendTime    = Utils::Byte::Get3Bytes(value, 0u);
			}
		}
		if (profile.transportWideCcId != 0u)
		{
			uint8_t length{ 0u };
			auto* value = packet->GetExtension(profile.transportWideCcId, length);
			if (value && length == 2u)
			{
				snapshot.hasTransportWideCc = true;
				snapshot.transportWideCc    = Utils::Byte::Get2Bytes(value, 0u);
			}
		}

		return snapshot;
	}

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
		std::vector<uint8_t> buffer{ 0x80, PayloadType, 0x00, 0x00, 0x00, 0x00,
			                           0x00, 0x00,        0x00, 0x00, 0x00, 0x00 };

		buffer.insert(buffer.end(), payload.begin(), payload.end());

		return buffer;
	}

	const FBS::Transport::ConsumeRequest* BuildConsumeRequest(
	  flatbuffers::FlatBufferBuilder& builder,
	  const MediaFixture& fixture,
	  const ConsumerConfig& config  = {},
	  FBS::RtpParameters::Type type = FBS::RtpParameters::Type::SIMPLE)
	{
		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> codecParameters;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> rtcpFeedback;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpHeaderExtensionParameters>> headerExtensions;
		const bool isAudio  = std::strncmp(fixture.mimeType, "audio/", 6u) == 0;
		const auto channels = isAudio ? flatbuffers::Optional<uint8_t>(2u)
		                              : flatbuffers::Optional<uint8_t>(flatbuffers::nullopt);

		if (config.useNack)
		{
			rtcpFeedback.emplace_back(FBS::RtpParameters::CreateRtcpFeedbackDirect(builder, "nack"));
		}

		if (config.midId != 0u)
		{
			headerExtensions.emplace_back(
			  FBS::RtpParameters::CreateRtpHeaderExtensionParametersDirect(
			    builder, FBS::RtpParameters::RtpHeaderExtensionUri::Mid, config.midId));
		}

		if (config.absSendTimeId != 0u)
		{
			headerExtensions.emplace_back(
			  FBS::RtpParameters::CreateRtpHeaderExtensionParametersDirect(
			    builder, FBS::RtpParameters::RtpHeaderExtensionUri::AbsSendTime, config.absSendTimeId));
		}

		if (config.transportWideCcId != 0u)
		{
			headerExtensions.emplace_back(
			  FBS::RtpParameters::CreateRtpHeaderExtensionParametersDirect(
			    builder,
			    FBS::RtpParameters::RtpHeaderExtensionUri::TransportWideCcDraft01,
			    config.transportWideCcId));
		}

		auto codec = FBS::RtpParameters::CreateRtpCodecParametersDirect(
		  builder,
		  fixture.mimeType,
		  PayloadType,
		  isAudio ? 48000u : 90000u,
		  channels,
		  &codecParameters,
		  &rtcpFeedback);

		auto consumerEncoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(config.ssrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType),
		  0,
		  false,
		  config.scalabilityMode);

		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpCodecParameters>> codecs{ codec };
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> encodings{
			consumerEncoding
		};

		auto rtcp          = FBS::RtpParameters::CreateRtcpParametersDirect(builder, "consumer-cname");
		auto rtpParameters = FBS::RtpParameters::CreateRtpParametersDirect(
		  builder, config.mid[0] == '\0' ? nullptr : config.mid, &codecs, &headerExtensions, &encodings, rtcp);

		auto consumableEncoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(ProducerSsrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType),
		  0,
		  false,
		  config.scalabilityMode);
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> consumableRtpEncodings{
			consumableEncoding
		};

		auto request = FBS::Transport::CreateConsumeRequestDirect(
		  builder,
		  config.consumerId,
		  config.producerId,
		  isAudio ? FBS::RtpParameters::MediaKind::AUDIO : FBS::RtpParameters::MediaKind::VIDEO,
		  rtpParameters,
		  type,
		  &consumableRtpEncodings);

		builder.Finish(request);

		return flatbuffers::GetRoot<FBS::Transport::ConsumeRequest>(builder.GetBufferPointer());
	}

	class TestConsumerListener : public RTC::Consumer::Listener
	{
	public:
		explicit TestConsumerListener(const MediaFixture& fixture, ExtensionProfile extensionProfile = {})
		  : fixture(fixture), extensionProfile(extensionProfile)
		{
		}

		void OnConsumerSendRtpPacket(RTC::Consumer* /*consumer*/, RTC::RtpPacket* packet) override
		{
			this->sentNalTypes.push_back(this->fixture.nalType(packet->GetPayload()));
			this->sentPackets.emplace_back(CapturePacket(packet, this->extensionProfile));

			if (this->throwOnSend)
			{
				throw std::runtime_error("injected consumer send failure");
			}
		}

		void OnConsumerRetransmitRtpPacket(RTC::Consumer* /*consumer*/, RTC::RtpPacket* packet) override
		{
			this->retransmittedPackets.emplace_back(CapturePacket(packet, this->extensionProfile));
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
		ExtensionProfile extensionProfile;
		std::vector<uint8_t> sentNalTypes;
		std::vector<PacketSnapshot> sentPackets;
		std::vector<PacketSnapshot> retransmittedPackets;
		size_t keyFrameRequests{ 0u };
		bool throwOnSend{ false };
	};

	class MutatingPayloadDescriptorHandler : public RTC::Codecs::PayloadDescriptorHandler
	{
	public:
		enum class Result
		{
			REJECT,
			THROW
		};

		explicit MutatingPayloadDescriptorHandler(Result result) : result(result)
		{
		}

		void Dump() const override
		{
		}

		bool Process(
		  RTC::Codecs::EncodingContext* /*context*/, uint8_t* data, bool& /*marker*/) override
		{
			this->originalByte = data[0];
			this->processed    = true;
			data[0] ^= 0x5au;

			if (this->result == Result::THROW)
			{
				throw std::runtime_error("injected payload processing failure");
			}

			return false;
		}

		void Restore(uint8_t* data) noexcept override
		{
			if (this->processed)
			{
				data[0]         = this->originalByte;
				this->processed = false;
			}
		}

		uint8_t GetSpatialLayer() const override
		{
			return 0u;
		}

		uint8_t GetTemporalLayer() const override
		{
			return 0u;
		}

		bool IsKeyFrame() const override
		{
			return false;
		}

	private:
		Result result;
		uint8_t originalByte{ 0u };
		bool processed{ false };
	};

	class PacketPreservingConsumer : public RTC::SimpleConsumer
	{
	public:
		PacketPreservingConsumer(
		  RTC::Shared* shared,
		  const std::string& id,
		  const std::string& producerId,
		  RTC::Consumer::Listener* listener,
		  const FBS::Transport::ConsumeRequest* data,
		  ExtensionProfile extensionProfile)
		  : RTC::SimpleConsumer(shared, id, producerId, listener, data),
		    extensionProfile(extensionProfile)
		{
		}

		void SendRtpPacket(
		  RTC::RtpPacket* packet, RTC::Consumer::RtpPacketFanoutContext& /*fanoutContext*/) override
		{
			this->receivedPackets.emplace_back(CapturePacket(packet, this->extensionProfile));
			const auto originalSsrc = packet->GetSsrc();
			const auto originalSeq  = packet->GetSequenceNumber();

			packet->SetSsrc(this->GetRtpParameters().encodings[0].ssrc);
			packet->SetSequenceNumber(originalSeq + 100u);
			packet->UpdateAbsSendTime(123456u);
			packet->UpdateTransportWideCc01(4321u);
			this->sentPackets.emplace_back(CapturePacket(packet, this->extensionProfile));

			// Match the existing non-Simple Consumer contract: packet identity fields
			// are restored, while transport-updated extension values are left in place.
			packet->SetSsrc(originalSsrc);
			packet->SetSequenceNumber(originalSeq);
		}

	public:
		ExtensionProfile extensionProfile;
		std::vector<PacketSnapshot> receivedPackets;
		std::vector<PacketSnapshot> sentPackets;
	};

	class SharedPacketMutatingConsumer : public RTC::SimpleConsumer
	{
	public:
		using RTC::SimpleConsumer::SimpleConsumer;

		void SendRtpPacket(RTC::RtpPacket* packet, RTC::Consumer::RtpPacketFanoutContext& fanoutContext) override
		{
			this->mutatedPacket.reset(packet->Clone());
			this->mutatedPacket->SetMarker(!packet->HasMarker());
			REQUIRE(this->mutatedPacket->GetPayloadLength() > 0u);
			this->mutatedPacket->GetPayload()[0] ^= 0x1fu;
			this->mutatedBytes.assign(
			  this->mutatedPacket->GetData(),
			  this->mutatedPacket->GetData() + this->mutatedPacket->GetSize());
			fanoutContext.sharedPacket = this->mutatedPacket;
		}

	public:
		std::shared_ptr<RTC::RtpPacket> mutatedPacket;
		std::vector<uint8_t> mutatedBytes;
	};

	class TestRtpStreamRecvListener : public RTC::RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RTC::RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
		}

		void OnRtpStreamSendRtcpPacket(RTC::RtpStreamRecv* /*rtpStream*/, RTC::RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}
	};

	const FBS::Transport::ProduceRequest* BuildProduceRequest(
	  flatbuffers::FlatBufferBuilder& builder, const char* producerId)
	{
		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> codecParameters;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> rtcpFeedback;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpHeaderExtensionParameters>> headerExtensions;

		auto codec = FBS::RtpParameters::CreateRtpCodecParametersDirect(
		  builder,
		  H264Fixture.mimeType,
		  PayloadType,
		  90000u,
		  flatbuffers::nullopt,
		  &codecParameters,
		  &rtcpFeedback);
		auto encoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(ProducerSsrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType));

		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpCodecParameters>> codecs{ codec };
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> encodings{ encoding };
		auto rtcp          = FBS::RtpParameters::CreateRtcpParametersDirect(builder, "producer-cname");
		auto rtpParameters = FBS::RtpParameters::CreateRtpParametersDirect(
		  builder, "source", &codecs, &headerExtensions, &encodings, rtcp);

		auto codecMapping = FBS::RtpParameters::CreateCodecMapping(builder, PayloadType, PayloadType);
		auto encodingMapping = FBS::RtpParameters::CreateEncodingMappingDirect(
		  builder, nullptr, flatbuffers::Optional<uint32_t>(ProducerSsrc), nullptr, ProducerSsrc);
		std::vector<flatbuffers::Offset<FBS::RtpParameters::CodecMapping>> codecMappings{ codecMapping };
		std::vector<flatbuffers::Offset<FBS::RtpParameters::EncodingMapping>> encodingMappings{
			encodingMapping
		};
		auto rtpMapping =
		  FBS::RtpParameters::CreateRtpMappingDirect(builder, &codecMappings, &encodingMappings);

		auto request = FBS::Transport::CreateProduceRequestDirect(
		  builder, producerId, FBS::RtpParameters::MediaKind::VIDEO, rtpParameters, rtpMapping);

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
		void OnProducerSendRtcpPacket(RTC::Producer* /*producer*/, RTC::RTCP::Packet* /*packet*/) override
		{
		}
		void OnProducerNeedWorstRemoteFractionLost(
		  RTC::Producer* /*producer*/, uint32_t /*mappedSsrc*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}
	};

	class TestRouterListener : public RTC::Router::Listener
	{
	public:
		RTC::WebRtcServer* OnRouterNeedWebRtcServer(
		  RTC::Router* /*router*/, std::string& /*webRtcServerId*/) override
		{
			return nullptr;
		}
	};

	class TestRtpObserver : public RTC::RtpObserver
	{
	public:
		TestRtpObserver(
		  RTC::Shared* shared,
		  const std::string& id,
		  RTC::RtpObserver::Listener* listener,
		  ExtensionProfile profile)
		  : RTC::RtpObserver(shared, id, listener), profile(profile)
		{
		}

		void AddProducer(RTC::Producer* /*producer*/) override
		{
		}
		void RemoveProducer(RTC::Producer* /*producer*/) override
		{
		}
		void ReceiveRtpPacket(RTC::Producer* /*producer*/, RTC::RtpPacket* packet) override
		{
			this->packets.emplace_back(CapturePacket(packet, this->profile));
		}
		void ProducerPaused(RTC::Producer* /*producer*/) override
		{
		}
		void ProducerResumed(RTC::Producer* /*producer*/) override
		{
		}

	protected:
		void Paused() override
		{
		}
		void Resumed() override
		{
		}

	public:
		ExtensionProfile profile;
		std::vector<PacketSnapshot> packets;
	};

	struct CanonicalPacket
	{
		explicit CanonicalPacket(uint16_t sequenceNumber = 5000u, uint32_t timestamp = 90000u)
		  : CanonicalPacket(H264Fixture, { 0x65u, 0xaau }, sequenceNumber, timestamp)
		{
		}

		CanonicalPacket(
		  const MediaFixture& fixture,
		  std::vector<uint8_t> payload,
		  uint16_t sequenceNumber = 5000u,
		  uint32_t timestamp      = 90000u)
		{
			const size_t packetLength = RTC::RtpPacket::HeaderSize + payload.size();
			REQUIRE(packetLength <= this->buffer.size());

			this->buffer[0] = 0x80u;
			this->buffer[1] = PayloadType;
			std::memcpy(this->buffer.data() + RTC::RtpPacket::HeaderSize, payload.data(), payload.size());

			this->packet.reset(
			  RTC::RtpPacket::Parse(this->buffer.data(), packetLength, this->buffer.size()));
			REQUIRE(this->packet);

			this->packet->SetPayloadType(PayloadType);
			this->packet->SetSequenceNumber(sequenceNumber);
			this->packet->SetTimestamp(timestamp);
			this->packet->SetSsrc(ProducerSsrc);
			this->packet->SetMarker(true);
			fixture.processRtpPacket(this->packet.get());

			std::array<uint8_t, RTC::MidMaxLength> mid{ 's', 'o', 'u', 'r', 'c', 'e', '0', '0' };
			std::array<uint8_t, 3u> absSendTime{ 0x12u, 0x34u, 0x56u };
			std::array<uint8_t, 2u> transportWideCc{ 0x78u, 0x9au };
			std::vector<RTC::RtpPacket::GenericExtension> extensions{
				{ static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
				  static_cast<uint8_t>(mid.size()),
				  mid.data() },
				{ static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
				  static_cast<uint8_t>(absSendTime.size()),
				  absSendTime.data() },
				{ static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01),
				  static_cast<uint8_t>(transportWideCc.size()),
				  transportWideCc.data() }
			};

			REQUIRE(this->packet->SetExtensions(1u, extensions));
			this->packet->SetMidExtensionId(static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID));
			this->packet->SetAbsSendTimeExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME));
			this->packet->SetTransportWideCc01ExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01));

			this->bytes.assign(this->packet->GetData(), this->packet->GetData() + this->packet->GetSize());
		}

		void SetCanonicalMid(const std::string& value)
		{
			REQUIRE(this->packet->UpdateMid(value));
			this->mid = value;
			this->bytes.assign(this->packet->GetData(), this->packet->GetData() + this->packet->GetSize());
		}

		std::array<uint8_t, 1600u> buffer{};
		std::unique_ptr<RTC::RtpPacket> packet;
		std::string mid{ "source00" };
		std::vector<uint8_t> bytes;
	};

	void RequirePacketProfile(
	  const PacketSnapshot& snapshot,
	  const ConsumerConfig& config,
	  const std::vector<uint8_t>& expectedExtensionIds)
	{
		CHECK(snapshot.ssrc == config.ssrc);
		CHECK(snapshot.mid == config.mid);
		CHECK(snapshot.extensionIds == expectedExtensionIds);
		CHECK(snapshot.hasAbsSendTime == (config.absSendTimeId != 0u));
		CHECK(snapshot.hasTransportWideCc == (config.transportWideCcId != 0u));
	}

	void RequireCanonicalPacket(const CanonicalPacket& canonicalPacket)
	{
		CHECK(
		  std::vector<uint8_t>(
		    canonicalPacket.packet->GetData(),
		    canonicalPacket.packet->GetData() + canonicalPacket.packet->GetSize()) ==
		  canonicalPacket.bytes);

		std::string mid;
		uint32_t absSendTime{ 0u };
		uint16_t transportWideCc{ 0u };
		REQUIRE(canonicalPacket.packet->ReadMid(mid));
		REQUIRE(canonicalPacket.packet->ReadAbsSendTime(absSendTime));
		REQUIRE(canonicalPacket.packet->ReadTransportWideCc01(transportWideCc));
		CHECK(mid == canonicalPacket.mid);
		CHECK(absSendTime == 0x123456u);
		CHECK(transportWideCc == 0x789au);
	}

	std::vector<uint8_t> CapturePayload(const RTC::RtpPacket* packet)
	{
		if (packet->GetPayloadLength() == 0u)
		{
			return {};
		}

		return { packet->GetPayload(), packet->GetPayload() + packet->GetPayloadLength() };
	}

	void RequestRetransmission(RTC::Consumer& consumer, uint32_t mediaSsrc, uint16_t sequenceNumber)
	{
		RTC::RTCP::FeedbackRtpNackPacket nackPacket(/*senderSsrc*/ 0u, mediaSsrc);
		nackPacket.AddItem(new RTC::RTCP::FeedbackRtpNackItem(sequenceNumber, 0u));
		consumer.ReceiveNack(&nackPacket);
	}

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

		RTC::Consumer::RtpPacketFanoutContext fanoutContext;
		consumer.SendRtpPacket(packet.get(), fanoutContext);
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
} // namespace

SCENARIO("SimpleConsumer forwards H265 parameter sets while waiting for sync", "[consumer][h265]")
{
	ChannelMessageRegistrator* registrator = new ChannelMessageRegistrator();
	RTC::Shared shared(registrator, nullptr);
	TestConsumerListener consumerListener(H265Fixture);

	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H265Fixture);
	RTC::SimpleConsumer consumer(
	  &shared, "consumer-h265-sync", "producer-h265-sync", &consumerListener, request);

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

TEST_CASE(
  "SimpleConsumer isolates outbound extension profiles and NACK packets",
  "[consumer][rtp][extensions][nack]")
{
	constexpr const char* ProducerId{ "producer-extension-isolation" };
	const ConsumerConfig configA{
		"consumer-extension-a", ProducerId, "alpha", 22222222u, 1u, 4u, 5u, true
	};
	const ConsumerConfig configB{
		"consumer-extension-b", ProducerId, "bravo", 33333333u, 18u, 19u, 20u, true
	};
	const ConsumerConfig configC{
		"consumer-extension-c", ProducerId, "cider", 44444444u, 18u, 19u, 20u, true
	};

	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestRouterListener routerListener;
	RTC::Router router(&shared, "router-extension-isolation", &routerListener);

	TestProducerListener producerListener;
	flatbuffers::FlatBufferBuilder producerBuilder;
	const auto* producerRequest = BuildProduceRequest(producerBuilder, ProducerId);
	RTC::Producer producer(&shared, ProducerId, &producerListener, producerRequest);
	router.OnTransportNewProducer(nullptr, &producer);
	TestRtpObserver observer(
	  &shared,
	  "observer-extension-isolation",
	  &router,
	  { static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
	    static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
	    static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01) });
	router.OnRtpObserverAddProducer(&observer, &producer);

	TestConsumerListener listenerA(
	  H264Fixture, { configA.midId, configA.absSendTimeId, configA.transportWideCcId });
	flatbuffers::FlatBufferBuilder consumerBuilderA;
	const auto* consumerRequestA = BuildConsumeRequest(consumerBuilderA, H264Fixture, configA);
	RTC::SimpleConsumer consumerA(&shared, configA.consumerId, ProducerId, &listenerA, consumerRequestA);

	TestConsumerListener listenerB(
	  H264Fixture, { configB.midId, configB.absSendTimeId, configB.transportWideCcId });
	flatbuffers::FlatBufferBuilder consumerBuilderB;
	const auto* consumerRequestB = BuildConsumeRequest(consumerBuilderB, H264Fixture, configB);
	RTC::SimpleConsumer consumerB(&shared, configB.consumerId, ProducerId, &listenerB, consumerRequestB);
	TestConsumerListener listenerC(
	  H264Fixture, { configC.midId, configC.absSendTimeId, configC.transportWideCcId });
	flatbuffers::FlatBufferBuilder consumerBuilderC;
	const auto* consumerRequestC = BuildConsumeRequest(consumerBuilderC, H264Fixture, configC);
	RTC::SimpleConsumer consumerC(&shared, configC.consumerId, ProducerId, &listenerC, consumerRequestC);

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

	SetupActiveSyncConsumer(consumerA, producerStream);
	SetupActiveSyncConsumer(consumerB, producerStream);
	SetupActiveSyncConsumer(consumerC, producerStream);
	router.OnTransportNewConsumer(nullptr, &consumerA, ProducerId);
	router.OnTransportNewConsumer(nullptr, &consumerB, ProducerId);
	router.OnTransportNewConsumer(nullptr, &consumerC, ProducerId);

	CanonicalPacket canonicalPacket;
	router.OnTransportProducerRtpPacketReceived(nullptr, &producer, canonicalPacket.packet.get());

	REQUIRE(listenerA.sentPackets.size() == 1u);
	REQUIRE(listenerB.sentPackets.size() == 1u);
	REQUIRE(listenerC.sentPackets.size() == 1u);
	RequirePacketProfile(listenerA.sentPackets[0], configA, { 1u, 4u, 5u });
	RequirePacketProfile(listenerB.sentPackets[0], configB, { 18u, 19u, 20u });
	RequirePacketProfile(listenerC.sentPackets[0], configC, { 18u, 19u, 20u });
	CHECK(listenerA.sentPackets[0].data == canonicalPacket.packet->GetData());
	CHECK(listenerB.sentPackets[0].data != canonicalPacket.packet->GetData());
	CHECK(listenerC.sentPackets[0].data != canonicalPacket.packet->GetData());
	CHECK(listenerB.sentPackets[0].data == listenerC.sentPackets[0].data);
	CHECK(listenerA.sentPackets[0].data != listenerB.sentPackets[0].data);
	RequireCanonicalPacket(canonicalPacket);
	REQUIRE(observer.packets.size() == 1u);
	CHECK(observer.packets[0].bytes == canonicalPacket.bytes);
	CHECK(observer.packets[0].mid == "source00");
	CHECK(observer.packets[0].absSendTime == 0x123456u);
	CHECK(observer.packets[0].transportWideCc == 0x789au);

	RequestRetransmission(consumerA, configA.ssrc, listenerA.sentPackets[0].sequenceNumber);
	RequestRetransmission(consumerB, configB.ssrc, listenerB.sentPackets[0].sequenceNumber);
	RequestRetransmission(consumerC, configC.ssrc, listenerC.sentPackets[0].sequenceNumber);

	REQUIRE(listenerA.retransmittedPackets.size() == 1u);
	REQUIRE(listenerB.retransmittedPackets.size() == 1u);
	REQUIRE(listenerC.retransmittedPackets.size() == 1u);
	RequirePacketProfile(listenerA.retransmittedPackets[0], configA, { 1u, 4u, 5u });
	RequirePacketProfile(listenerB.retransmittedPackets[0], configB, { 18u, 19u, 20u });
	RequirePacketProfile(listenerC.retransmittedPackets[0], configC, { 18u, 19u, 20u });
	CHECK(listenerA.retransmittedPackets[0].sequenceNumber == listenerA.sentPackets[0].sequenceNumber);
	CHECK(listenerB.retransmittedPackets[0].sequenceNumber == listenerB.sentPackets[0].sequenceNumber);
	CHECK(listenerC.retransmittedPackets[0].sequenceNumber == listenerC.sentPackets[0].sequenceNumber);
	CHECK(listenerA.retransmittedPackets[0].data != listenerA.sentPackets[0].data);
	CHECK(listenerB.retransmittedPackets[0].data == listenerB.sentPackets[0].data);
	CHECK(listenerC.retransmittedPackets[0].data == listenerC.sentPackets[0].data);
	CHECK(listenerB.retransmittedPackets[0].data == listenerC.retransmittedPackets[0].data);
	CHECK(listenerA.retransmittedPackets[0].data != listenerB.retransmittedPackets[0].data);
	CHECK(listenerA.retransmittedPackets[0].bytes == listenerA.sentPackets[0].bytes);
	CHECK(listenerB.retransmittedPackets[0].bytes == listenerB.sentPackets[0].bytes);
	CHECK(listenerC.retransmittedPackets[0].bytes == listenerC.sentPackets[0].bytes);
	CHECK(listenerA.retransmittedPackets[0].absSendTime == listenerA.sentPackets[0].absSendTime);
	CHECK(listenerA.retransmittedPackets[0].transportWideCc == listenerA.sentPackets[0].transportWideCc);
	CHECK(listenerB.retransmittedPackets[0].absSendTime == listenerB.sentPackets[0].absSendTime);
	CHECK(listenerB.retransmittedPackets[0].transportWideCc == listenerB.sentPackets[0].transportWideCc);
	RequireCanonicalPacket(canonicalPacket);
}

TEST_CASE(
  "SimpleConsumers rewrite semantically permuted extension IDs and share the matching profile",
  "[consumer][rtp][extensions][nack][permutation]")
{
	struct Permutation
	{
		const char* ingressMid;
		const char* consumerMidA;
		const char* consumerMidB;
		uint8_t midId;
		uint8_t absSendTimeId;
		uint8_t transportWideCcId;
	};

	const std::array<Permutation, 2u> permutations{ Permutation{ "10", "10", "20", 5u, 4u, 1u },
		                                              Permutation{ "100", "100", "200", 4u, 1u, 5u } };

	for (const auto& permutation : permutations)
	{
		CAPTURE(
		  permutation.ingressMid,
		  permutation.midId,
		  permutation.absSendTimeId,
		  permutation.transportWideCcId);
		constexpr const char* ProducerId{ "producer-extension-permutation" };
		const ConsumerConfig configA{ "consumer-extension-permutation-a",
			                            ProducerId,
			                            permutation.consumerMidA,
			                            22222222u,
			                            permutation.midId,
			                            permutation.absSendTimeId,
			                            permutation.transportWideCcId,
			                            true };
		const ConsumerConfig configB{ "consumer-extension-permutation-b",
			                            ProducerId,
			                            permutation.consumerMidB,
			                            33333333u,
			                            permutation.midId,
			                            permutation.absSendTimeId,
			                            permutation.transportWideCcId,
			                            true };

		RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-extension-permutation", &routerListener);

		TestConsumerListener listenerA(
		  H264Fixture, { configA.midId, configA.absSendTimeId, configA.transportWideCcId });
		flatbuffers::FlatBufferBuilder builderA;
		const auto* requestA = BuildConsumeRequest(builderA, H264Fixture, configA);
		RTC::SimpleConsumer consumerA(&shared, configA.consumerId, ProducerId, &listenerA, requestA);

		TestConsumerListener listenerB(
		  H264Fixture, { configB.midId, configB.absSendTimeId, configB.transportWideCcId });
		flatbuffers::FlatBufferBuilder builderB;
		const auto* requestB = BuildConsumeRequest(builderB, H264Fixture, configB);
		RTC::SimpleConsumer consumerB(&shared, configB.consumerId, ProducerId, &listenerB, requestB);

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
		SetupActiveSyncConsumer(consumerA, producerStream);
		SetupActiveSyncConsumer(consumerB, producerStream);

		CanonicalPacket canonicalPacket;
		canonicalPacket.SetCanonicalMid(permutation.ingressMid);
		router.SendRtpPacketToConsumersForTesting(
		  canonicalPacket.packet.get(), { &consumerA, &consumerB });

		REQUIRE(listenerA.sentPackets.size() == 1u);
		REQUIRE(listenerB.sentPackets.size() == 1u);
		RequirePacketProfile(listenerA.sentPackets[0], configA, { 1u, 4u, 5u });
		RequirePacketProfile(listenerB.sentPackets[0], configB, { 1u, 4u, 5u });
		CHECK(listenerA.sentPackets[0].data != canonicalPacket.packet->GetData());
		CHECK(listenerA.sentPackets[0].data == listenerB.sentPackets[0].data);
		RequireCanonicalPacket(canonicalPacket);

		RequestRetransmission(consumerA, configA.ssrc, listenerA.sentPackets[0].sequenceNumber);
		RequestRetransmission(consumerB, configB.ssrc, listenerB.sentPackets[0].sequenceNumber);

		REQUIRE(listenerA.retransmittedPackets.size() == 1u);
		REQUIRE(listenerB.retransmittedPackets.size() == 1u);
		RequirePacketProfile(listenerA.retransmittedPackets[0], configA, { 1u, 4u, 5u });
		RequirePacketProfile(listenerB.retransmittedPackets[0], configB, { 1u, 4u, 5u });
		CHECK(listenerA.retransmittedPackets[0].bytes == listenerA.sentPackets[0].bytes);
		CHECK(listenerB.retransmittedPackets[0].bytes == listenerB.sentPackets[0].bytes);
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "SimpleConsumers do not share extension profiles with different URI to ID mappings",
  "[consumer][rtp][extensions][nack][permutation]")
{
	constexpr const char* ProducerId{ "producer-extension-profile-permutation" };
	const ConsumerConfig configA{
		"consumer-extension-profile-a", ProducerId, "100", 22222222u, 8u, 9u, 10u, true
	};
	const ConsumerConfig configB{
		"consumer-extension-profile-b", ProducerId, "200", 33333333u, 9u, 8u, 10u, true
	};

	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestRouterListener routerListener;
	RTC::Router router(&shared, "router-extension-profile-permutation", &routerListener);

	TestConsumerListener listenerA(
	  H264Fixture, { configA.midId, configA.absSendTimeId, configA.transportWideCcId });
	flatbuffers::FlatBufferBuilder builderA;
	const auto* requestA = BuildConsumeRequest(builderA, H264Fixture, configA);
	RTC::SimpleConsumer consumerA(&shared, configA.consumerId, ProducerId, &listenerA, requestA);

	TestConsumerListener listenerB(
	  H264Fixture, { configB.midId, configB.absSendTimeId, configB.transportWideCcId });
	flatbuffers::FlatBufferBuilder builderB;
	const auto* requestB = BuildConsumeRequest(builderB, H264Fixture, configB);
	RTC::SimpleConsumer consumerB(&shared, configB.consumerId, ProducerId, &listenerB, requestB);

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
	SetupActiveSyncConsumer(consumerA, producerStream);
	SetupActiveSyncConsumer(consumerB, producerStream);

	CanonicalPacket canonicalPacket;
	router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), { &consumerA, &consumerB });

	REQUIRE(listenerA.sentPackets.size() == 1u);
	REQUIRE(listenerB.sentPackets.size() == 1u);
	RequirePacketProfile(listenerA.sentPackets[0], configA, { 8u, 9u, 10u });
	RequirePacketProfile(listenerB.sentPackets[0], configB, { 8u, 9u, 10u });
	CHECK(listenerA.sentPackets[0].data != listenerB.sentPackets[0].data);
	RequireCanonicalPacket(canonicalPacket);

	RequestRetransmission(consumerA, configA.ssrc, listenerA.sentPackets[0].sequenceNumber);
	RequestRetransmission(consumerB, configB.ssrc, listenerB.sentPackets[0].sequenceNumber);

	REQUIRE(listenerA.retransmittedPackets.size() == 1u);
	REQUIRE(listenerB.retransmittedPackets.size() == 1u);
	RequirePacketProfile(listenerA.retransmittedPackets[0], configA, { 8u, 9u, 10u });
	RequirePacketProfile(listenerB.retransmittedPackets[0], configB, { 8u, 9u, 10u });
	CHECK(listenerA.retransmittedPackets[0].bytes == listenerA.sentPackets[0].bytes);
	CHECK(listenerB.retransmittedPackets[0].bytes == listenerB.sentPackets[0].bytes);
	RequireCanonicalPacket(canonicalPacket);
}

TEST_CASE(
  "SimpleConsumer rejects a payload-mutated shared retransmission candidate in both fanout orders",
  "[consumer][rtp][extensions][nack]")
{
	for (const bool mutatingFirst : { false, true })
	{
		CAPTURE(mutatingFirst);
		constexpr const char* ProducerId{ "producer-shared-payload-provenance" };
		const ConsumerConfig simpleConfig{
			"consumer-shared-payload-simple",
			ProducerId,
			"simple00",
			22222222u,
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01),
			true
		};
		const ConsumerConfig mutatingConfig{
			"consumer-shared-payload-mutating",
			ProducerId,
			"mutator0",
			33333333u,
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01),
			false
		};

		RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-shared-payload-provenance", &routerListener);

		TestConsumerListener simpleListener(
		  H264Fixture,
		  { simpleConfig.midId, simpleConfig.absSendTimeId, simpleConfig.transportWideCcId });
		flatbuffers::FlatBufferBuilder simpleBuilder;
		const auto* simpleRequest = BuildConsumeRequest(simpleBuilder, H264Fixture, simpleConfig);
		RTC::SimpleConsumer simpleConsumer(
		  &shared, simpleConfig.consumerId, ProducerId, &simpleListener, simpleRequest);

		TestConsumerListener mutatingListener(H264Fixture);
		flatbuffers::FlatBufferBuilder mutatingBuilder;
		const auto* mutatingRequest = BuildConsumeRequest(mutatingBuilder, H264Fixture, mutatingConfig);
		SharedPacketMutatingConsumer mutatingConsumer(
		  &shared, mutatingConfig.consumerId, ProducerId, &mutatingListener, mutatingRequest);

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
		SetupActiveSyncConsumer(simpleConsumer, producerStream);

		CanonicalPacket canonicalPacket;
		const std::vector<RTC::Consumer*> consumers =
		  mutatingFirst ? std::vector<RTC::Consumer*>{ &mutatingConsumer, &simpleConsumer }
		                : std::vector<RTC::Consumer*>{ &simpleConsumer, &mutatingConsumer };
		router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), consumers);

		REQUIRE(simpleListener.sentPackets.size() == 1u);
		REQUIRE(mutatingConsumer.mutatedPacket);
		CHECK(mutatingConsumer.mutatedBytes != simpleListener.sentPackets[0].bytes);
		RequestRetransmission(
		  simpleConsumer, simpleConfig.ssrc, simpleListener.sentPackets[0].sequenceNumber);
		REQUIRE(simpleListener.retransmittedPackets.size() == 1u);
		CHECK(simpleListener.retransmittedPackets[0].bytes == simpleListener.sentPackets[0].bytes);
		CHECK(simpleListener.retransmittedPackets[0].data != mutatingConsumer.mutatedPacket->GetData());
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "SimpleConsumers isolate different long one-byte MIDs in both fanout orders and NACK",
  "[consumer][rtp][extensions][nack][mid]")
{
	constexpr const char* ProducerId{ "producer-long-mid-isolation" };
	const ConsumerConfig configA{
		"consumer-long-mid-a", ProducerId, "custom-mid", 22222222u, 1u, 4u, 5u, true
	};
	const ConsumerConfig configB{
		"consumer-long-mid-b", ProducerId, "second-mid", 33333333u, 1u, 4u, 5u, true
	};

	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestRouterListener routerListener;
	RTC::Router router(&shared, "router-long-mid-isolation", &routerListener);

	TestConsumerListener listenerA(H264Fixture, { 1u, 4u, 5u });
	flatbuffers::FlatBufferBuilder builderA;
	const auto* requestA = BuildConsumeRequest(builderA, H264Fixture, configA);
	RTC::SimpleConsumer consumerA(&shared, configA.consumerId, ProducerId, &listenerA, requestA);

	TestConsumerListener listenerB(H264Fixture, { 1u, 4u, 5u });
	flatbuffers::FlatBufferBuilder builderB;
	const auto* requestB = BuildConsumeRequest(builderB, H264Fixture, configB);
	RTC::SimpleConsumer consumerB(&shared, configB.consumerId, ProducerId, &listenerB, requestB);

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
	SetupActiveSyncConsumer(consumerA, producerStream);
	SetupActiveSyncConsumer(consumerB, producerStream);

	CanonicalPacket packetAFirst(5100u, 90000u);
	router.SendRtpPacketToConsumersForTesting(packetAFirst.packet.get(), { &consumerA, &consumerB });
	CanonicalPacket packetBFirst(5101u, 93000u);
	router.SendRtpPacketToConsumersForTesting(packetBFirst.packet.get(), { &consumerB, &consumerA });

	REQUIRE(listenerA.sentPackets.size() == 2u);
	REQUIRE(listenerB.sentPackets.size() == 2u);
	for (size_t index{ 0u }; index < 2u; ++index)
	{
		RequirePacketProfile(listenerA.sentPackets[index], configA, { 1u, 4u, 5u });
		RequirePacketProfile(listenerB.sentPackets[index], configB, { 1u, 4u, 5u });
		CHECK(listenerA.sentPackets[index].hasOneByteExtensions);
		CHECK_FALSE(listenerA.sentPackets[index].hasTwoBytesExtensions);
		CHECK(listenerB.sentPackets[index].hasOneByteExtensions);
		CHECK_FALSE(listenerB.sentPackets[index].hasTwoBytesExtensions);
		CHECK(listenerA.sentPackets[index].data == listenerB.sentPackets[index].data);
	}
	CHECK(listenerA.sentPackets[0].data != packetAFirst.packet->GetData());
	CHECK(listenerA.sentPackets[1].data != packetBFirst.packet->GetData());
	RequireCanonicalPacket(packetAFirst);
	RequireCanonicalPacket(packetBFirst);

	RequestRetransmission(consumerB, configB.ssrc, listenerB.sentPackets[0].sequenceNumber);
	RequestRetransmission(consumerA, configA.ssrc, listenerA.sentPackets[0].sequenceNumber);
	RequestRetransmission(consumerA, configA.ssrc, listenerA.sentPackets[1].sequenceNumber);
	RequestRetransmission(consumerB, configB.ssrc, listenerB.sentPackets[1].sequenceNumber);

	REQUIRE(listenerA.retransmittedPackets.size() == 2u);
	REQUIRE(listenerB.retransmittedPackets.size() == 2u);
	for (size_t index{ 0u }; index < 2u; ++index)
	{
		RequirePacketProfile(listenerA.retransmittedPackets[index], configA, { 1u, 4u, 5u });
		RequirePacketProfile(listenerB.retransmittedPackets[index], configB, { 1u, 4u, 5u });
		CHECK(listenerA.retransmittedPackets[index].hasOneByteExtensions);
		CHECK_FALSE(listenerA.retransmittedPackets[index].hasTwoBytesExtensions);
		CHECK(listenerB.retransmittedPackets[index].hasOneByteExtensions);
		CHECK_FALSE(listenerB.retransmittedPackets[index].hasTwoBytesExtensions);
		CHECK(listenerA.retransmittedPackets[index].bytes == listenerA.sentPackets[index].bytes);
		CHECK(listenerB.retransmittedPackets[index].bytes == listenerB.sentPackets[index].bytes);
	}
	RequireCanonicalPacket(packetAFirst);
	RequireCanonicalPacket(packetBFirst);
}

TEST_CASE(
  "SimpleConsumer rejects a raw-FBS MID longer than the RTP extension limit",
  "[consumer][rtp][mid][validation]")
{
	std::string oversizedMid(256u, 'm');
	const ConsumerConfig config{ "consumer-oversized-mid",
		                           "producer-oversized-mid",
		                           oversizedMid.c_str(),
		                           22222222u,
		                           1u,
		                           4u,
		                           5u,
		                           false };
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestConsumerListener listener(H264Fixture, { 1u, 4u, 5u });
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H264Fixture, config);

	try
	{
		RTC::SimpleConsumer consumer(
		  &shared, config.consumerId, config.producerId, &listener, request);
		FAIL("SimpleConsumer unexpectedly accepted a 256-byte MID");
	}
	catch (const MediaSoupTypeError& error)
	{
		CHECK(
		  std::string(error.what()).find("RTP MID exceeds encodable extension length") !=
		  std::string::npos);
	}
}

TEST_CASE(
  "layered Consumers reject unsupported long MID during construction",
  "[consumer][rtp][mid][validation]")
{
	const ConsumerConfig config{ "consumer-layered-long-mid",
		                           "producer-layered-long-mid",
		                           "custom-mid",
		                           22222222u,
		                           1u,
		                           4u,
		                           5u,
		                           false };
	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestConsumerListener listener(H264Fixture, { 1u, 4u, 5u });
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H264Fixture, config);

	const auto requireLongMidRejection = [&](auto&& construct)
	{
		try
		{
			construct();
			FAIL("layered Consumer unexpectedly accepted a long MID");
		}
		catch (const MediaSoupTypeError& error)
		{
			CHECK(
			  std::string(error.what()).find("rtpParameters.mid exceeds the supported length") !=
			  std::string::npos);
		}
	};

	requireLongMidRejection(
	  [&]
	  {
		  RTC::SimulcastConsumer consumer(
		    &shared, config.consumerId, config.producerId, &listener, request);
	  });
	requireLongMidRejection(
	  [&]
	  { RTC::SvcConsumer consumer(&shared, config.consumerId, config.producerId, &listener, request); });
}

TEST_CASE(
  "PipeConsumers share an isolated immutable fanout clone and restore NACK identity",
  "[consumer][pipe][rtp][extensions][nack]")
{
	for (const bool simpleFirst : { false, true })
	{
		CAPTURE(simpleFirst);
		constexpr const char* ProducerId{ "producer-pipe-profile-isolation" };
		const ConsumerConfig pipeConfigA{
			"consumer-pipe-profile-a", ProducerId, "", 22222222u, 0u, 0u, 0u, true
		};
		const ConsumerConfig pipeConfigB{
			"consumer-pipe-profile-b", ProducerId, "", 33333333u, 0u, 0u, 0u, true
		};
		const ConsumerConfig simpleConfig{
			"consumer-pipe-profile-simple", ProducerId, "simple00", 44444444u, 8u, 9u, 10u, true
		};
		const ExtensionProfile canonicalProfile{
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
			static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01)
		};

		RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-pipe-profile-isolation", &routerListener);

		TestConsumerListener pipeListenerA(H264Fixture, canonicalProfile);
		flatbuffers::FlatBufferBuilder pipeBuilderA;
		const auto* pipeRequestA =
		  BuildConsumeRequest(pipeBuilderA, H264Fixture, pipeConfigA, FBS::RtpParameters::Type::PIPE);
		RTC::PipeConsumer pipeConsumerA(
		  &shared, pipeConfigA.consumerId, ProducerId, &pipeListenerA, pipeRequestA);

		TestConsumerListener pipeListenerB(H264Fixture, canonicalProfile);
		flatbuffers::FlatBufferBuilder pipeBuilderB;
		const auto* pipeRequestB =
		  BuildConsumeRequest(pipeBuilderB, H264Fixture, pipeConfigB, FBS::RtpParameters::Type::PIPE);
		RTC::PipeConsumer pipeConsumerB(
		  &shared, pipeConfigB.consumerId, ProducerId, &pipeListenerB, pipeRequestB);

		TestConsumerListener simpleListener(
		  H264Fixture,
		  { simpleConfig.midId, simpleConfig.absSendTimeId, simpleConfig.transportWideCcId });
		flatbuffers::FlatBufferBuilder simpleBuilder;
		const auto* simpleRequest = BuildConsumeRequest(simpleBuilder, H264Fixture, simpleConfig);
		RTC::SimpleConsumer simpleConsumer(
		  &shared, simpleConfig.consumerId, ProducerId, &simpleListener, simpleRequest);

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
		SetupActiveSyncConsumer(simpleConsumer, producerStream);
		pipeConsumerA.TransportConnected();
		pipeConsumerB.TransportConnected();

		CanonicalPacket canonicalPacket;
		const std::vector<RTC::Consumer*> consumers =
		  simpleFirst ? std::vector<RTC::Consumer*>{ &simpleConsumer, &pipeConsumerA, &pipeConsumerB }
		              : std::vector<RTC::Consumer*>{ &pipeConsumerB, &pipeConsumerA, &simpleConsumer };
		router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), consumers);

		REQUIRE(pipeListenerA.sentPackets.size() == 1u);
		REQUIRE(pipeListenerB.sentPackets.size() == 1u);
		REQUIRE(simpleListener.sentPackets.size() == 1u);
		CHECK(pipeListenerA.sentPackets[0].data == canonicalPacket.packet->GetData());
		CHECK(pipeListenerB.sentPackets[0].data == canonicalPacket.packet->GetData());
		CHECK(simpleListener.sentPackets[0].data != canonicalPacket.packet->GetData());
		CHECK(pipeListenerA.sentPackets[0].ssrc == pipeConfigA.ssrc);
		CHECK(pipeListenerB.sentPackets[0].ssrc == pipeConfigB.ssrc);
		CHECK(pipeListenerA.sentPackets[0].mid == "source00");
		CHECK(pipeListenerB.sentPackets[0].mid == "source00");
		RequirePacketProfile(simpleListener.sentPackets[0], simpleConfig, { 8u, 9u, 10u });
		RequireCanonicalPacket(canonicalPacket);

		RequestRetransmission(
		  pipeConsumerB, pipeConfigB.ssrc, pipeListenerB.sentPackets[0].sequenceNumber);
		RequestRetransmission(
		  simpleConsumer, simpleConfig.ssrc, simpleListener.sentPackets[0].sequenceNumber);
		RequestRetransmission(
		  pipeConsumerA, pipeConfigA.ssrc, pipeListenerA.sentPackets[0].sequenceNumber);

		REQUIRE(pipeListenerA.retransmittedPackets.size() == 1u);
		REQUIRE(pipeListenerB.retransmittedPackets.size() == 1u);
		REQUIRE(simpleListener.retransmittedPackets.size() == 1u);
		CHECK(pipeListenerA.retransmittedPackets[0].data == pipeListenerB.retransmittedPackets[0].data);
		CHECK(pipeListenerA.retransmittedPackets[0].data != simpleListener.retransmittedPackets[0].data);
		CHECK(pipeListenerA.retransmittedPackets[0].bytes == pipeListenerA.sentPackets[0].bytes);
		CHECK(pipeListenerB.retransmittedPackets[0].bytes == pipeListenerB.sentPackets[0].bytes);
		CHECK(simpleListener.retransmittedPackets[0].bytes == simpleListener.sentPackets[0].bytes);
		CHECK(pipeListenerA.retransmittedPackets[0].ssrc == pipeConfigA.ssrc);
		CHECK(pipeListenerB.retransmittedPackets[0].ssrc == pipeConfigB.ssrc);
		CHECK(
		  pipeListenerA.retransmittedPackets[0].sequenceNumber ==
		  pipeListenerA.sentPackets[0].sequenceNumber);
		CHECK(
		  pipeListenerB.retransmittedPackets[0].sequenceNumber ==
		  pipeListenerB.sentPackets[0].sequenceNumber);
		RequirePacketProfile(simpleListener.retransmittedPackets[0], simpleConfig, { 8u, 9u, 10u });
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "Opus SimpleConsumers share one immutable retransmission clone", "[consumer][rtp][extensions][nack]")
{
	constexpr const char* ProducerId{ "producer-opus-shared-profile" };
	const ConsumerConfig configA{
		"consumer-opus-shared-a", ProducerId, "audio000", 22222222u, 1u, 4u, 5u, true
	};
	const ConsumerConfig configB{
		"consumer-opus-shared-b", ProducerId, "audio111", 33333333u, 1u, 4u, 5u, true
	};

	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestRouterListener routerListener;
	RTC::Router router(&shared, "router-opus-shared-profile", &routerListener);

	TestConsumerListener listenerA(
	  OpusFixture, { configA.midId, configA.absSendTimeId, configA.transportWideCcId });
	flatbuffers::FlatBufferBuilder builderA;
	const auto* requestA = BuildConsumeRequest(builderA, OpusFixture, configA);
	RTC::SimpleConsumer consumerA(&shared, configA.consumerId, ProducerId, &listenerA, requestA);

	TestConsumerListener listenerB(
	  OpusFixture, { configB.midId, configB.absSendTimeId, configB.transportWideCcId });
	flatbuffers::FlatBufferBuilder builderB;
	const auto* requestB = BuildConsumeRequest(builderB, OpusFixture, configB);
	RTC::SimpleConsumer consumerB(&shared, configB.consumerId, ProducerId, &listenerB, requestB);

	TestRtpStreamRecvListener rtpStreamRecvListener;
	RTC::RtpStream::Params producerParams;
	producerParams.ssrc        = ProducerSsrc;
	producerParams.payloadType = PayloadType;
	producerParams.clockRate   = 48000u;
	producerParams.mimeType.SetMimeType(OpusFixture.mimeType);
	RTC::RtpStreamRecv producerStream(
	  &rtpStreamRecvListener,
	  producerParams,
	  /*sendNackDelayMs*/ 0u,
	  /*useRtpInactivityCheck*/ false);
	SetupActiveSyncConsumer(consumerA, producerStream);
	SetupActiveSyncConsumer(consumerB, producerStream);

	CanonicalPacket canonicalPacket(OpusFixture, { 0x00u, 0xaau });
	router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), { &consumerA, &consumerB });

	REQUIRE(listenerA.sentPackets.size() == 1u);
	REQUIRE(listenerB.sentPackets.size() == 1u);
	CHECK(listenerA.sentPackets[0].data == canonicalPacket.packet->GetData());
	CHECK(listenerB.sentPackets[0].data == canonicalPacket.packet->GetData());
	RequestRetransmission(consumerA, configA.ssrc, listenerA.sentPackets[0].sequenceNumber);
	RequestRetransmission(consumerB, configB.ssrc, listenerB.sentPackets[0].sequenceNumber);
	REQUIRE(listenerA.retransmittedPackets.size() == 1u);
	REQUIRE(listenerB.retransmittedPackets.size() == 1u);
	CHECK(listenerA.retransmittedPackets[0].data == listenerB.retransmittedPackets[0].data);
	CHECK(listenerA.retransmittedPackets[0].bytes == listenerA.sentPackets[0].bytes);
	CHECK(listenerB.retransmittedPackets[0].bytes == listenerB.sentPackets[0].bytes);
	RequirePacketProfile(listenerA.retransmittedPackets[0], configA, { 1u, 4u, 5u });
	RequirePacketProfile(listenerB.retransmittedPackets[0], configB, { 1u, 4u, 5u });
	RequireCanonicalPacket(canonicalPacket);
}

TEST_CASE(
  "Router mixed consumer fanout preserves the canonical RTP packet",
  "[router][consumer][rtp][extensions]")
{
	constexpr const char* ProducerId{ "producer-mixed-extension-isolation" };
	const ConsumerConfig preservingConfig{
		"consumer-preserving",
		ProducerId,
		"plain",
		22222222u,
		static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
		static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
		static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01),
		false
	};
	const ConsumerConfig simpleConfig{
		"consumer-simple-rewrite", ProducerId, "simple", 33333333u, 8u, 9u, 10u, true
	};

	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestRouterListener routerListener;
	RTC::Router router(&shared, "router-mixed-extension-isolation", &routerListener);

	TestConsumerListener preservingListener(H264Fixture);
	flatbuffers::FlatBufferBuilder preservingBuilder;
	const auto* preservingRequest =
	  BuildConsumeRequest(preservingBuilder, H264Fixture, preservingConfig);
	PacketPreservingConsumer preservingConsumer(
	  &shared,
	  preservingConfig.consumerId,
	  ProducerId,
	  &preservingListener,
	  preservingRequest,
	  { preservingConfig.midId, preservingConfig.absSendTimeId, preservingConfig.transportWideCcId });

	TestConsumerListener simpleListener(
	  H264Fixture, { simpleConfig.midId, simpleConfig.absSendTimeId, simpleConfig.transportWideCcId });
	flatbuffers::FlatBufferBuilder simpleBuilder;
	const auto* simpleRequest = BuildConsumeRequest(simpleBuilder, H264Fixture, simpleConfig);
	RTC::SimpleConsumer simpleConsumer(
	  &shared, simpleConfig.consumerId, ProducerId, &simpleListener, simpleRequest);

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

	SetupActiveSyncConsumer(simpleConsumer, producerStream);
	CanonicalPacket canonicalPacket;
	router.SendRtpPacketToConsumersForTesting(
	  canonicalPacket.packet.get(), { &preservingConsumer, &simpleConsumer });

	REQUIRE(preservingConsumer.sentPackets.size() == 1u);
	REQUIRE(simpleListener.sentPackets.size() == 1u);
	RequirePacketProfile(preservingConsumer.sentPackets[0], preservingConfig, { 1u, 4u, 5u });
	RequirePacketProfile(simpleListener.sentPackets[0], simpleConfig, { 8u, 9u, 10u });
	CHECK(preservingConsumer.sentPackets[0].data == canonicalPacket.packet->GetData());
	CHECK(simpleListener.sentPackets[0].data != canonicalPacket.packet->GetData());
	RequireCanonicalPacket(canonicalPacket);

	preservingConsumer.sentPackets.clear();
	simpleListener.sentPackets.clear();
	CanonicalPacket reverseCanonicalPacket(5001u, 93000u);
	router.SendRtpPacketToConsumersForTesting(
	  reverseCanonicalPacket.packet.get(), { &simpleConsumer, &preservingConsumer });
	REQUIRE(preservingConsumer.sentPackets.size() == 1u);
	REQUIRE(simpleListener.sentPackets.size() == 1u);
	RequirePacketProfile(preservingConsumer.sentPackets[0], preservingConfig, { 1u, 4u, 5u });
	RequirePacketProfile(simpleListener.sentPackets[0], simpleConfig, { 8u, 9u, 10u });
	RequireCanonicalPacket(reverseCanonicalPacket);

	RequestRetransmission(
	  simpleConsumer, simpleConfig.ssrc, simpleListener.sentPackets[0].sequenceNumber);
	REQUIRE(simpleListener.retransmittedPackets.size() == 1u);
	RequirePacketProfile(simpleListener.retransmittedPackets[0], simpleConfig, { 8u, 9u, 10u });
	RequireCanonicalPacket(reverseCanonicalPacket);
}

TEST_CASE("SimpleConsumer extension preparation is transactional", "[consumer][rtp][extensions][failure]")
{
	constexpr const char* ProducerId{ "producer-extension-transaction" };
	const ConsumerConfig config{
		"consumer-extension-transaction", ProducerId, "video", 33333333u, 18u, 19u, 20u, false
	};

	RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
	TestConsumerListener listener(
	  H264Fixture, { config.midId, config.absSendTimeId, config.transportWideCcId });
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildConsumeRequest(builder, H264Fixture, config);
	RTC::SimpleConsumer consumer(&shared, config.consumerId, ProducerId, &listener, request);

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

	std::array<uint8_t, 1600u> largeBuffer{};
	constexpr size_t LargePacketSize{ 1590u };
	largeBuffer[0]                          = 0x80u;
	largeBuffer[1]                          = PayloadType;
	largeBuffer[RTC::RtpPacket::HeaderSize] = 0x65u;
	std::unique_ptr<RTC::RtpPacket> largePacket(
	  RTC::RtpPacket::Parse(largeBuffer.data(), LargePacketSize, largeBuffer.size()));
	REQUIRE(largePacket);
	largePacket->SetPayloadType(PayloadType);
	largePacket->SetSequenceNumber(6000u);
	largePacket->SetTimestamp(90000u);
	largePacket->SetSsrc(ProducerSsrc);
	largePacket->SetMarker(true);
	H264Fixture.processRtpPacket(largePacket.get());
	RTC::Consumer::RtpPacketFanoutContext fanoutContext;
	consumer.SendRtpPacket(largePacket.get(), fanoutContext);
	CHECK(listener.sentPackets.empty());

	// The failed extension rebuild must not commit sync or sequence state.
	SendH264NalUnit(consumer, 6001u, 93000u, 1u);
	CHECK(listener.sentPackets.empty());
	SendH264NalUnit(consumer, 6002u, 96000u, 5u);
	REQUIRE(listener.sentPackets.size() == 1u);
	RequirePacketProfile(listener.sentPackets[0], config, { 18u, 19u, 20u });

	ConsumerConfig customMidConfig = config;
	customMidConfig.consumerId     = "consumer-custom-mid";
	customMidConfig.mid            = "custom-mid";
	customMidConfig.useNack        = true;
	TestConsumerListener customMidListener(
	  H264Fixture,
	  { customMidConfig.midId, customMidConfig.absSendTimeId, customMidConfig.transportWideCcId });
	flatbuffers::FlatBufferBuilder customMidBuilder;
	const auto* customMidRequest = BuildConsumeRequest(customMidBuilder, H264Fixture, customMidConfig);
	RTC::SimpleConsumer customMidConsumer(
	  &shared, customMidConfig.consumerId, ProducerId, &customMidListener, customMidRequest);
	SetupActiveSyncConsumer(customMidConsumer, producerStream);
	CanonicalPacket customMidPacket(7000u, 120000u);
	RTC::Consumer::RtpPacketFanoutContext customMidFanoutContext;
	customMidConsumer.SendRtpPacket(customMidPacket.packet.get(), customMidFanoutContext);
	REQUIRE(customMidListener.sentPackets.size() == 1u);
	RequirePacketProfile(customMidListener.sentPackets[0], customMidConfig, { 18u, 19u, 20u });
	RequestRetransmission(
	  customMidConsumer, customMidConfig.ssrc, customMidListener.sentPackets[0].sequenceNumber);
	REQUIRE(customMidListener.retransmittedPackets.size() == 1u);
	RequirePacketProfile(customMidListener.retransmittedPackets[0], customMidConfig, { 18u, 19u, 20u });
}

TEST_CASE(
  "SimpleConsumer restores canonical RTP after payload processing rejects or throws",
  "[consumer][rtp][exception-restore][payload]")
{
	for (const auto result : { MutatingPayloadDescriptorHandler::Result::REJECT,
	                           MutatingPayloadDescriptorHandler::Result::THROW })
	{
		CAPTURE(result == MutatingPayloadDescriptorHandler::Result::THROW);
		const ConsumerConfig config{
			"consumer-payload-restore",
			"producer-payload-restore",
			"source00",
			ConsumerSsrc,
			1u,
			4u,
			5u,
			false
		};
		RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
		TestConsumerListener listener(
		  OpusFixture, { config.midId, config.absSendTimeId, config.transportWideCcId });
		flatbuffers::FlatBufferBuilder builder;
		const auto* request = BuildConsumeRequest(builder, OpusFixture, config);
		RTC::SimpleConsumer consumer(
		  &shared, config.consumerId, config.producerId, &listener, request);

		TestRtpStreamRecvListener rtpStreamRecvListener;
		RTC::RtpStream::Params producerParams;
		producerParams.ssrc        = ProducerSsrc;
		producerParams.payloadType = PayloadType;
		producerParams.clockRate   = 48000u;
		producerParams.mimeType.SetMimeType(OpusFixture.mimeType);
		RTC::RtpStreamRecv producerStream(
		  &rtpStreamRecvListener,
		  producerParams,
		  /*sendNackDelayMs*/ 0u,
		  /*useRtpInactivityCheck*/ false);
		SetupActiveSyncConsumer(consumer, producerStream);

		CanonicalPacket canonicalPacket(OpusFixture, { 0x00u, 0xaau });
		canonicalPacket.packet->SetPayloadDescriptorHandler(
		  std::make_shared<MutatingPayloadDescriptorHandler>(result));
		RTC::Consumer::RtpPacketFanoutContext fanoutContext;

		if (result == MutatingPayloadDescriptorHandler::Result::THROW)
		{
			CHECK_THROWS_AS(
			  consumer.SendRtpPacket(canonicalPacket.packet.get(), fanoutContext), std::runtime_error);
		}
		else
		{
			CHECK_NOTHROW(consumer.SendRtpPacket(canonicalPacket.packet.get(), fanoutContext));
		}

		CHECK(listener.sentPackets.empty());
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "SimpleConsumer restores canonical RTP when the send listener throws in both fanout orders",
  "[consumer][rtp][extensions][nack][exception-restore]")
{
	for (const bool throwingConsumerFirst : { false, true })
	{
		CAPTURE(throwingConsumerFirst);
		constexpr const char* ProducerId{ "producer-simple-exception-restore" };
		const ConsumerConfig simpleConfig{
			"consumer-simple-exception-restore",
			ProducerId,
			"source00",
			22222222u,
			1u,
			4u,
			5u,
			true
		};
		const ConsumerConfig preservingConfig{
			"consumer-before-simple-exception",
			ProducerId,
			"source00",
			33333333u,
			1u,
			4u,
			5u,
			false
		};

		RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-simple-exception-restore", &routerListener);

		TestConsumerListener simpleListener(
		  H264Fixture,
		  { simpleConfig.midId, simpleConfig.absSendTimeId, simpleConfig.transportWideCcId });
		simpleListener.throwOnSend = true;
		flatbuffers::FlatBufferBuilder simpleBuilder;
		const auto* simpleRequest = BuildConsumeRequest(simpleBuilder, H264Fixture, simpleConfig);
		RTC::SimpleConsumer simpleConsumer(
		  &shared, simpleConfig.consumerId, ProducerId, &simpleListener, simpleRequest);

		TestConsumerListener preservingListener(H264Fixture);
		flatbuffers::FlatBufferBuilder preservingBuilder;
		const auto* preservingRequest =
		  BuildConsumeRequest(preservingBuilder, H264Fixture, preservingConfig);
		PacketPreservingConsumer preservingConsumer(
		  &shared,
		  preservingConfig.consumerId,
		  ProducerId,
		  &preservingListener,
		  preservingRequest,
		  { preservingConfig.midId,
		    preservingConfig.absSendTimeId,
		    preservingConfig.transportWideCcId });

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
		SetupActiveSyncConsumer(simpleConsumer, producerStream);

		CanonicalPacket canonicalPacket(
		  static_cast<uint16_t>(5100u + (throwingConsumerFirst ? 1u : 0u)), 90000u);
		const std::vector<RTC::Consumer*> consumers =
		  throwingConsumerFirst
		    ? std::vector<RTC::Consumer*>{ &simpleConsumer, &preservingConsumer }
		    : std::vector<RTC::Consumer*>{ &preservingConsumer, &simpleConsumer };

		CHECK_NOTHROW(
		  router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), consumers));
		REQUIRE(simpleListener.sentPackets.size() == 1u);
		REQUIRE(preservingConsumer.receivedPackets.size() == 1u);
		CHECK(preservingConsumer.receivedPackets[0].bytes == canonicalPacket.bytes);
		RequireCanonicalPacket(canonicalPacket);

		simpleListener.throwOnSend = false;
		RequestRetransmission(
		  simpleConsumer, simpleConfig.ssrc, simpleListener.sentPackets[0].sequenceNumber);
		REQUIRE(simpleListener.retransmittedPackets.size() == 1u);
		CHECK(simpleListener.retransmittedPackets[0].bytes == simpleListener.sentPackets[0].bytes);
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "PipeConsumer restores canonical RTP when the send listener throws in both fanout orders",
  "[consumer][pipe][rtp][nack][exception-restore]")
{
	for (const bool throwingConsumerFirst : { false, true })
	{
		CAPTURE(throwingConsumerFirst);
		constexpr const char* ProducerId{ "producer-pipe-exception-restore" };
		const ConsumerConfig pipeConfig{
			"consumer-pipe-exception-restore", ProducerId, "", 22222222u, 0u, 0u, 0u, true
		};
		const ConsumerConfig preservingConfig{
			"consumer-before-pipe-exception",
			ProducerId,
			"source00",
			33333333u,
			1u,
			4u,
			5u,
			false
		};

		RTC::Shared shared(new ChannelMessageRegistrator(), nullptr);
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-pipe-exception-restore", &routerListener);

		TestConsumerListener pipeListener(
		  H264Fixture,
		  { static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID),
		    static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME),
		    static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01) });
		pipeListener.throwOnSend = true;
		flatbuffers::FlatBufferBuilder pipeBuilder;
		const auto* pipeRequest =
		  BuildConsumeRequest(pipeBuilder, H264Fixture, pipeConfig, FBS::RtpParameters::Type::PIPE);
		RTC::PipeConsumer pipeConsumer(
		  &shared, pipeConfig.consumerId, ProducerId, &pipeListener, pipeRequest);
		pipeConsumer.TransportConnected();

		TestConsumerListener preservingListener(H264Fixture);
		flatbuffers::FlatBufferBuilder preservingBuilder;
		const auto* preservingRequest =
		  BuildConsumeRequest(preservingBuilder, H264Fixture, preservingConfig);
		PacketPreservingConsumer preservingConsumer(
		  &shared,
		  preservingConfig.consumerId,
		  ProducerId,
		  &preservingListener,
		  preservingRequest,
		  { preservingConfig.midId,
		    preservingConfig.absSendTimeId,
		    preservingConfig.transportWideCcId });

		CanonicalPacket canonicalPacket(
		  static_cast<uint16_t>(5200u + (throwingConsumerFirst ? 1u : 0u)), 93000u);
		const std::vector<RTC::Consumer*> consumers =
		  throwingConsumerFirst
		    ? std::vector<RTC::Consumer*>{ &pipeConsumer, &preservingConsumer }
		    : std::vector<RTC::Consumer*>{ &preservingConsumer, &pipeConsumer };

		CHECK_NOTHROW(
		  router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), consumers));
		REQUIRE(pipeListener.sentPackets.size() == 1u);
		REQUIRE(preservingConsumer.receivedPackets.size() == 1u);
		CHECK(preservingConsumer.receivedPackets[0].bytes == canonicalPacket.bytes);
		RequireCanonicalPacket(canonicalPacket);

		pipeListener.throwOnSend = false;
		RequestRetransmission(
		  pipeConsumer, pipeConfig.ssrc, pipeListener.sentPackets[0].sequenceNumber);
		REQUIRE(pipeListener.retransmittedPackets.size() == 1u);
		CHECK(pipeListener.retransmittedPackets[0].bytes == pipeListener.sentPackets[0].bytes);
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "VP8 SimulcastConsumer restores payload-mutated canonical RTP when the send listener throws",
  "[consumer][simulcast][rtp][nack][exception-restore]")
{
	for (const bool throwingConsumerFirst : { false, true })
	{
		CAPTURE(throwingConsumerFirst);
		constexpr const char* ProducerId{ "producer-simulcast-exception-restore" };
		const ConsumerConfig simulcastConfig{
			"consumer-simulcast-exception-restore",
			ProducerId,
			"source00",
			22222222u,
			1u,
			4u,
			5u,
			true,
			"L1T2"
		};
		const ConsumerConfig preservingConfig{
			"consumer-before-simulcast-exception",
			ProducerId,
			"source00",
			33333333u,
			1u,
			4u,
			5u,
			false
		};

		Channel::ChannelSocket channel(
		  NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
		RTC::Shared shared(
		  new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-simulcast-exception-restore", &routerListener);

		TestConsumerListener simulcastListener(
		  VP8Fixture,
		  { simulcastConfig.midId,
		    simulcastConfig.absSendTimeId,
		    simulcastConfig.transportWideCcId });
		simulcastListener.throwOnSend = true;
		flatbuffers::FlatBufferBuilder simulcastBuilder;
		const auto* simulcastRequest = BuildConsumeRequest(
		  simulcastBuilder, VP8Fixture, simulcastConfig, FBS::RtpParameters::Type::SIMULCAST);
		RTC::SimulcastConsumer simulcastConsumer(
		  &shared, simulcastConfig.consumerId, ProducerId, &simulcastListener, simulcastRequest);

		TestConsumerListener preservingListener(VP8Fixture);
		flatbuffers::FlatBufferBuilder preservingBuilder;
		const auto* preservingRequest =
		  BuildConsumeRequest(preservingBuilder, VP8Fixture, preservingConfig);
		PacketPreservingConsumer preservingConsumer(
		  &shared,
		  preservingConfig.consumerId,
		  ProducerId,
		  &preservingListener,
		  preservingRequest,
		  { preservingConfig.midId,
		    preservingConfig.absSendTimeId,
		    preservingConfig.transportWideCcId });

		TestRtpStreamRecvListener rtpStreamRecvListener;
		RTC::RtpStream::Params producerParams;
		producerParams.ssrc           = ProducerSsrc;
		producerParams.payloadType    = PayloadType;
		producerParams.clockRate      = 90000u;
		producerParams.spatialLayers  = 1u;
		producerParams.temporalLayers = 2u;
		producerParams.mimeType.SetMimeType(VP8Fixture.mimeType);
		RTC::RtpStreamRecv producerStream(
		  &rtpStreamRecvListener,
		  producerParams,
		  /*sendNackDelayMs*/ 0u,
		  /*useRtpInactivityCheck*/ false);
		const std::vector<uint8_t> producerScores{ 10u };
		simulcastConsumer.ProducerRtpStreamScores(&producerScores);
		simulcastConsumer.ProducerRtpStream(&producerStream, ProducerSsrc);
		simulcastConsumer.TransportConnected();

		CanonicalPacket canonicalPacket(
		  VP8Fixture,
		  { 0x90u, 0xe0u, 0x80u, 0x7bu, 0x21u, 0x20u, 0x00u, 0xaau },
		  static_cast<uint16_t>(5300u + (throwingConsumerFirst ? 1u : 0u)),
		  96000u);
		const auto canonicalPayload = CapturePayload(canonicalPacket.packet.get());
		const std::vector<RTC::Consumer*> consumers =
		  throwingConsumerFirst
		    ? std::vector<RTC::Consumer*>{ &simulcastConsumer, &preservingConsumer }
		    : std::vector<RTC::Consumer*>{ &preservingConsumer, &simulcastConsumer };

		CHECK_NOTHROW(
		  router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), consumers));
		REQUIRE(simulcastListener.sentPackets.size() == 1u);
		REQUIRE(preservingConsumer.receivedPackets.size() == 1u);
		CHECK(preservingConsumer.receivedPackets[0].bytes == canonicalPacket.bytes);
		CHECK(simulcastListener.sentPackets[0].payload != canonicalPayload);
		RequireCanonicalPacket(canonicalPacket);

		simulcastListener.throwOnSend = false;
		RequestRetransmission(
		  simulcastConsumer,
		  simulcastConfig.ssrc,
		  simulcastListener.sentPackets[0].sequenceNumber);
		REQUIRE(simulcastListener.retransmittedPackets.size() == 1u);
		CHECK(
		  simulcastListener.retransmittedPackets[0].payload ==
		  simulcastListener.sentPackets[0].payload);
		CHECK(
		  simulcastListener.retransmittedPackets[0].bytes ==
		  simulcastListener.sentPackets[0].bytes);
		RequireCanonicalPacket(canonicalPacket);
	}
}

TEST_CASE(
  "SvcConsumer restores canonical RTP when the send listener throws in both fanout orders",
  "[consumer][svc][rtp][nack][exception-restore]")
{
	for (const bool throwingConsumerFirst : { false, true })
	{
		CAPTURE(throwingConsumerFirst);
		constexpr const char* ProducerId{ "producer-svc-exception-restore" };
		const ConsumerConfig svcConfig{
			"consumer-svc-exception-restore",
			ProducerId,
			"source00",
			22222222u,
			1u,
			4u,
			5u,
			true,
			"L2T2"
		};
		const ConsumerConfig preservingConfig{
			"consumer-before-svc-exception",
			ProducerId,
			"source00",
			33333333u,
			1u,
			4u,
			5u,
			false
		};

		Channel::ChannelSocket channel(
		  NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
		RTC::Shared shared(
		  new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
		TestRouterListener routerListener;
		RTC::Router router(&shared, "router-svc-exception-restore", &routerListener);

		TestConsumerListener svcListener(
		  H264SvcFixture,
		  { svcConfig.midId, svcConfig.absSendTimeId, svcConfig.transportWideCcId });
		svcListener.throwOnSend = true;
		flatbuffers::FlatBufferBuilder svcBuilder;
		const auto* svcRequest = BuildConsumeRequest(
		  svcBuilder, H264SvcFixture, svcConfig, FBS::RtpParameters::Type::SVC);
		RTC::SvcConsumer svcConsumer(
		  &shared, svcConfig.consumerId, ProducerId, &svcListener, svcRequest);

		TestConsumerListener preservingListener(H264SvcFixture);
		flatbuffers::FlatBufferBuilder preservingBuilder;
		const auto* preservingRequest =
		  BuildConsumeRequest(preservingBuilder, H264SvcFixture, preservingConfig);
		PacketPreservingConsumer preservingConsumer(
		  &shared,
		  preservingConfig.consumerId,
		  ProducerId,
		  &preservingListener,
		  preservingRequest,
		  { preservingConfig.midId,
		    preservingConfig.absSendTimeId,
		    preservingConfig.transportWideCcId });

		TestRtpStreamRecvListener rtpStreamRecvListener;
		RTC::RtpStream::Params producerParams;
		producerParams.ssrc           = ProducerSsrc;
		producerParams.payloadType    = PayloadType;
		producerParams.clockRate      = 90000u;
		producerParams.spatialLayers  = 2u;
		producerParams.temporalLayers = 2u;
		producerParams.mimeType.SetMimeType(H264SvcFixture.mimeType);
		RTC::RtpStreamRecv producerStream(
		  &rtpStreamRecvListener,
		  producerParams,
		  /*sendNackDelayMs*/ 0u,
		  /*useRtpInactivityCheck*/ false);
		const std::vector<uint8_t> producerScores{ 10u };
		svcConsumer.ProducerRtpStreamScores(&producerScores);

		CanonicalPacket canonicalPacket(
		  H264SvcFixture,
		  { 0x85u, 0xb8u, 0x00u, 0x04u },
		  static_cast<uint16_t>(5400u + (throwingConsumerFirst ? 1u : 0u)),
		  99000u);
		REQUIRE(producerStream.ReceivePacket(canonicalPacket.packet.get()));
		RequireCanonicalPacket(canonicalPacket);
		svcConsumer.ProducerRtpStream(&producerStream, ProducerSsrc);
		svcConsumer.TransportConnected();

		const std::vector<RTC::Consumer*> consumers =
		  throwingConsumerFirst
		    ? std::vector<RTC::Consumer*>{ &svcConsumer, &preservingConsumer }
		    : std::vector<RTC::Consumer*>{ &preservingConsumer, &svcConsumer };
		CHECK_NOTHROW(
		  router.SendRtpPacketToConsumersForTesting(canonicalPacket.packet.get(), consumers));
		REQUIRE(svcListener.sentPackets.size() == 1u);
		REQUIRE(preservingConsumer.receivedPackets.size() == 1u);
		CHECK(preservingConsumer.receivedPackets[0].bytes == canonicalPacket.bytes);
		RequireCanonicalPacket(canonicalPacket);

		svcListener.throwOnSend = false;
		RequestRetransmission(
		  svcConsumer, svcConfig.ssrc, svcListener.sentPackets[0].sequenceNumber);
		REQUIRE(svcListener.retransmittedPackets.size() == 1u);
		CHECK(svcListener.retransmittedPackets[0].bytes == svcListener.sentPackets[0].bytes);
		RequireCanonicalPacket(canonicalPacket);
	}
}

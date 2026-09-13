#include "common.hpp"
#include "ChannelMessageRegistrator.hpp"
#include "DepLibUV.hpp"
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
#include <cstdlib> // setenv(), unsetenv()
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace
{
	constexpr uint8_t PayloadType{ 111u };
	constexpr uint8_t RtxPayloadType{ 112u };
	constexpr uint32_t ProducerSsrc{ 11111111u };
	constexpr uint32_t MappedSsrc{ 22222222u };
	constexpr uint32_t RtxSsrc{ 33333333u };

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

	const FBS::Transport::ProduceRequest* BuildProduceRequest(
	  flatbuffers::FlatBufferBuilder& builder,
	  uint32_t keyFrameRequestDelay = 0u,
	  bool withPliFeedback = false,
	  const char* mimeType = "video/VP8",
	  bool withRtx = false,
	  bool withFrameMarking = false)
	{
		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> codecParameters;
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> rtcpFeedback;

		if (withPliFeedback)
		{
			// PLI is negotiated as the "nack pli" RTCP feedback form.
			rtcpFeedback.emplace_back(FBS::RtpParameters::CreateRtcpFeedbackDirect(builder, "nack", "pli"));
		}
		if (withRtx)
		{
			rtcpFeedback.emplace_back(FBS::RtpParameters::CreateRtcpFeedbackDirect(builder, "nack", nullptr));
		}
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpHeaderExtensionParameters>> headerExtensions;
		headerExtensions.emplace_back(
		  FBS::RtpParameters::CreateRtpHeaderExtensionParametersDirect(
		    builder, FBS::RtpParameters::RtpHeaderExtensionUri::AbsCaptureTime, 9u));
		if (withFrameMarking)
		{
			headerExtensions.emplace_back(
			  FBS::RtpParameters::CreateRtpHeaderExtensionParametersDirect(
			    builder, FBS::RtpParameters::RtpHeaderExtensionUri::FrameMarking, 10u));
		}

		auto codec = FBS::RtpParameters::CreateRtpCodecParametersDirect(
		  builder, mimeType, PayloadType, 90000u, flatbuffers::nullopt, &codecParameters, &rtcpFeedback);
		auto rtxOffset = withRtx
		  ? FBS::RtpParameters::CreateRtx(builder, RtxSsrc)
		  : flatbuffers::Offset<FBS::RtpParameters::Rtx>{};
		auto encoding = FBS::RtpParameters::CreateRtpEncodingParametersDirect(
		  builder,
		  flatbuffers::Optional<uint32_t>(ProducerSsrc),
		  nullptr,
		  flatbuffers::Optional<uint8_t>(PayloadType),
		  rtxOffset);

		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpCodecParameters>> codecs{ codec };

		std::vector<flatbuffers::Offset<FBS::RtpParameters::Parameter>> rtxParameters;
		if (withRtx)
		{
			auto aptValue = FBS::RtpParameters::CreateInteger32(builder, PayloadType);
			rtxParameters.emplace_back(
			  FBS::RtpParameters::CreateParameterDirect(
			    builder, "apt", FBS::RtpParameters::Value::Integer32, aptValue.Union()));
			std::vector<flatbuffers::Offset<FBS::RtpParameters::RtcpFeedback>> noFeedback;
			codecs.emplace_back(
			  FBS::RtpParameters::CreateRtpCodecParametersDirect(
			    builder, "video/rtx", RtxPayloadType, 90000u, flatbuffers::nullopt, &rtxParameters, &noFeedback));
		}
		std::vector<flatbuffers::Offset<FBS::RtpParameters::RtpEncodingParameters>> encodings{ encoding };
		auto rtcp          = FBS::RtpParameters::CreateRtcpParametersDirect(builder, "producer-cname");
		auto rtpParameters = FBS::RtpParameters::CreateRtpParametersDirect(
		  builder, "video", &codecs, &headerExtensions, &encodings, rtcp);

		auto codecMapping = FBS::RtpParameters::CreateCodecMapping(builder, PayloadType, PayloadType);
		auto encodingMapping = FBS::RtpParameters::CreateEncodingMappingDirect(
		  builder, nullptr, flatbuffers::Optional<uint32_t>(ProducerSsrc), nullptr, MappedSsrc);
		std::vector<flatbuffers::Offset<FBS::RtpParameters::CodecMapping>> codecMappings{ codecMapping };
		if (withRtx)
		{
			codecMappings.emplace_back(
			  FBS::RtpParameters::CreateCodecMapping(builder, RtxPayloadType, RtxPayloadType));
		}
		std::vector<flatbuffers::Offset<FBS::RtpParameters::EncodingMapping>> encodingMappings{
			encodingMapping
		};
		auto rtpMapping =
		  FBS::RtpParameters::CreateRtpMappingDirect(builder, &codecMappings, &encodingMappings);

		auto request = FBS::Transport::CreateProduceRequestDirect(
		  builder,
		  "producer-rtcp-feedback",
		  FBS::RtpParameters::MediaKind::VIDEO,
		  rtpParameters,
		  rtpMapping,
		  keyFrameRequestDelay);

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
		  RTC::Producer* /*producer*/, RTC::RtpStreamRecv* rtpStream, uint32_t /*mappedSsrc*/) override
		{
			if (rtpStream)
			{
				this->rtpStreams.push_back(rtpStream);
			}
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
		std::vector<RTC::RtpStreamRecv*> rtpStreams;

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

	struct Vp8MediaPacket
	{
		Vp8MediaPacket(
		  uint16_t sequenceNumber, uint32_t timestamp, bool keyFrame, bool marker = true)
		{
			this->buffer[0]  = 0x80u;
			this->buffer[1]  = PayloadType;
			// Extended VP8 payload descriptor (X=1, S=1, partition 0), the
			// I/L/T/K extension-flags byte (all zero), then the VP8 frame header;
			// P bit 0 marks a key frame.
			this->buffer[12] = 0x90u;
			this->buffer[13] = 0x00u;
			this->buffer[14] = keyFrame ? 0x00u : 0x01u;
			this->packet.reset(
			  RTC::RtpPacket::Parse(
			    this->buffer.data(), RTC::RtpPacket::HeaderSize + 3u, this->buffer.size()));
			REQUIRE(this->packet);
			this->packet->SetPayloadType(PayloadType);
			this->packet->SetSequenceNumber(sequenceNumber);
			this->packet->SetTimestamp(timestamp);
			this->packet->SetSsrc(ProducerSsrc);
			this->packet->SetMarker(marker);
		}

		std::array<uint8_t, 1600u> buffer{};
		std::unique_ptr<RTC::RtpPacket> packet;
	};

	struct H264MediaPacket
	{
		// firstMb0=true writes first_mb_in_slice == 0 (ue(v) '1' bit) into the
		// slice header carried by the FU start fragment.  firstMb0=false writes
		// a non-zero first_mb (later slice of the same picture).
		H264MediaPacket(
		  uint16_t sequenceNumber,
		  uint32_t timestamp,
		  bool fragmentStart,
		  bool fragmentEnd,
		  bool marker,
		  bool frameMarking = false,
		  bool firstMb0 = false,
		  uint8_t nalType = 5u)
		{
			this->buffer[0] = 0x80u;
			this->buffer[1] = PayloadType;
			// H264 FU-A: NAL type 28, FU header with S/E and the original
			// NAL type (5 = IDR, 1 = non-IDR slice).
			this->buffer[12] = 28u;
			uint8_t fuHeader = nalType;
			if (fragmentStart)
			{
				fuHeader |= 0x80u;
			}
			if (fragmentEnd)
			{
				fuHeader |= 0x40u;
			}
			this->buffer[13] = fuHeader;
			size_t payloadLen = 2u;
			if (fragmentStart)
			{
				// Slice header first byte: first_mb_in_slice ue(v), 0 == '1'.
				this->buffer[14] = firstMb0 ? 0x80u : 0x00u;
				payloadLen       = 3u;
			}
			this->packet.reset(
			  RTC::RtpPacket::Parse(
			    this->buffer.data(), RTC::RtpPacket::HeaderSize + payloadLen, this->buffer.size()));
			REQUIRE(this->packet);
			this->packet->SetPayloadType(PayloadType);
			this->packet->SetSequenceNumber(sequenceNumber);
			this->packet->SetTimestamp(timestamp);
			this->packet->SetSsrc(ProducerSsrc);
			if (frameMarking)
			{
				uint8_t frameMarkingValue{ 0u };
				if (fragmentStart) frameMarkingValue |= 0x80u;
				if (fragmentEnd) frameMarkingValue |= 0x40u;
				if (fragmentStart) frameMarkingValue |= 0x20u;
				std::array<uint8_t, 3u> frameMarkingBytes{
					frameMarkingValue, 0u, 0u
				};
				std::vector<RTC::RtpPacket::GenericExtension> extensions{
					{10u, static_cast<uint8_t>(frameMarkingBytes.size()), frameMarkingBytes.data()}
				};
				REQUIRE(this->packet->SetExtensions(1u, extensions));
			}
			this->packet->SetMarker(marker);
		}

		std::array<uint8_t, 1600u> buffer{};
		std::unique_ptr<RTC::RtpPacket> packet;
	};

	struct H265MediaPacket
	{
		// firstSlice=true writes first_slice_segment_in_pic_flag=1 into the
		// slice segment header carried by the FU start fragment.  firstSlice
		// =false writes a later slice of the same picture.
		H265MediaPacket(
		  uint16_t sequenceNumber,
		  uint32_t timestamp,
		  bool fragmentStart,
		  bool fragmentEnd,
		  bool marker,
		  bool frameMarking = false,
		  bool firstSlice = false)
		{
			this->buffer[0] = 0x80u;
			this->buffer[1] = PayloadType;
			// H265 FU: payload HDR with NAL type 49, then FU header carrying
			// S/E and the original IRAP NAL type (IDR_W_RADL = 19).
			this->buffer[12] = 49u << 1;
			this->buffer[13] = 0x01u;
			uint8_t fuHeader = 19u;
			if (fragmentStart)
			{
				fuHeader |= 0x80u;
			}
			if (fragmentEnd)
			{
				fuHeader |= 0x40u;
			}
			this->buffer[14] = fuHeader;
			size_t payloadLen = 3u;
			if (fragmentStart)
			{
				// Slice segment header first byte.
				this->buffer[15] = firstSlice ? 0x80u : 0x00u;
				payloadLen       = 4u;
			}
			this->packet.reset(
			  RTC::RtpPacket::Parse(
			    this->buffer.data(), RTC::RtpPacket::HeaderSize + payloadLen, this->buffer.size()));
			REQUIRE(this->packet);
			this->packet->SetPayloadType(PayloadType);
			this->packet->SetSequenceNumber(sequenceNumber);
			this->packet->SetTimestamp(timestamp);
			this->packet->SetSsrc(ProducerSsrc);
			if (frameMarking)
			{
				uint8_t frameMarkingValue{ 0u };
				if (fragmentStart) frameMarkingValue |= 0x80u;
				if (fragmentEnd) frameMarkingValue |= 0x40u;
				if (fragmentStart) frameMarkingValue |= 0x20u;
				std::array<uint8_t, 3u> frameMarkingBytes{
					frameMarkingValue, 0u, 0u
				};
				std::vector<RTC::RtpPacket::GenericExtension> extensions{
					{10u, static_cast<uint8_t>(frameMarkingBytes.size()), frameMarkingBytes.data()}
				};
				REQUIRE(this->packet->SetExtensions(1u, extensions));
			}
			this->packet->SetMarker(marker);
		}

		std::array<uint8_t, 1600u> buffer{};
		std::unique_ptr<RTC::RtpPacket> packet;
	};

	struct Vp9MediaPacket
	{
		Vp9MediaPacket(
		  uint16_t sequenceNumber,
		  uint32_t timestamp,
		  bool start,
		  bool end,
		  bool marker,
		  bool keyFrame = true)
		{
			this->buffer[0] = 0x80u;
			this->buffer[1] = PayloadType;
			// I/P/L/F/B/E/V bits.  P=0 plus B=1 marks a key-frame start; E is
			// the codec-parsed frame-end bit.
			uint8_t descriptor = 0u;
			if (!keyFrame)
			{
				descriptor |= 0x40u;
			}
			if (start)
			{
				descriptor |= 0x08u;
			}
			if (end)
			{
				descriptor |= 0x04u;
			}
			this->buffer[12] = descriptor;
			this->packet.reset(
			  RTC::RtpPacket::Parse(
			    this->buffer.data(), RTC::RtpPacket::HeaderSize + 1u, this->buffer.size()));
			REQUIRE(this->packet);
			this->packet->SetPayloadType(PayloadType);
			this->packet->SetSequenceNumber(sequenceNumber);
			this->packet->SetTimestamp(timestamp);
			this->packet->SetSsrc(ProducerSsrc);
			this->packet->SetMarker(marker);
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

TEST_CASE(
  "Producer suppresses viewer key frame requests when cadence delay is enabled",
  "[producer][keyframe][cadence]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request =
	  BuildProduceRequest(builder, /*keyFrameRequestDelay=*/500u, /*withPliFeedback=*/true);
	RTC::Producer producer(&shared, "producer-cadence-suppression", &listener, request);

	// A non-key-frame packet creates the stream and triggers the existing
	// new-stream forced key frame request: exactly one PLI to the publisher.
	Vp8MediaPacket streamPacket(1u, 90000u, false);
	CHECK(
	  producer.ReceiveRtpPacket(streamPacket.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// A key frame clears the pending request state and refreshes the baseline.
	Vp8MediaPacket keyFramePacket(2u, 93600u, true);
	CHECK(
	  producer.ReceiveRtpPacket(keyFramePacket.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(keyFramePacket.packet->IsKeyFrame());
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// Viewer-originated PLI/FIR is suppressed and must not reach the publisher.
	producer.RequestKeyFrame(MappedSsrc, /*fromViewerRtcp=*/true);
	CHECK(listener.sentRtcpPackets.size() == 1u);

	// Let the coalescing window opened by the new-stream forced request expire
	// and pump the loop so its timer callback closes the window (in production
	// the resident loop does this continuously). DepLibUV::RunLoop() cannot be
	// used here: the test ChannelSocket keeps an always-active uv_async handle,
	// so UV_RUN_DEFAULT would never return.
	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	// Internal/service-originated requests still reach the publisher.
	producer.RequestKeyFrame(MappedSsrc, /*fromViewerRtcp=*/false);
	CHECK(listener.sentRtcpPackets.size() == 2u);
}

TEST_CASE(
  "Producer forwards viewer key frame requests when cadence delay is disabled",
  "[producer][keyframe][cadence]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request =
	  BuildProduceRequest(builder, /*keyFrameRequestDelay=*/0u, /*withPliFeedback=*/true);
	RTC::Producer producer(&shared, "producer-cadence-legacy", &listener, request);

	// Legacy mode (delay 0): the new-stream forced request fires once.
	Vp8MediaPacket streamPacket(1u, 90000u, false);
	CHECK(
	  producer.ReceiveRtpPacket(streamPacket.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// Clear the pending request state with a key frame.
	Vp8MediaPacket keyFramePacket(2u, 93600u, true);
	CHECK(
	  producer.ReceiveRtpPacket(keyFramePacket.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// Legacy behavior: viewer-originated requests are forwarded.
	producer.RequestKeyFrame(MappedSsrc, /*fromViewerRtcp=*/true);
	CHECK(listener.sentRtcpPackets.size() == 2u);
}

TEST_CASE(
  "Producer cadence watchdog requests exactly one key frame per interval",
  "[producer][keyframe][cadence]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request =
	  BuildProduceRequest(builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true);
	RTC::Producer producer(&shared, "producer-cadence-watchdog", &listener, request);

	// First non-key-frame packet: stream creation plus the existing new-stream
	// forced key frame request.
	Vp8MediaPacket first(1u, 90000u, false);
	CHECK(
	  producer.ReceiveRtpPacket(first.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// Packets within the interval must not trigger the watchdog.
	Vp8MediaPacket second(2u, 93600u, false);
	CHECK(
	  producer.ReceiveRtpPacket(second.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(listener.sentRtcpPackets.size() == 1u);

	// After the interval elapses, the next non-key-frame packet triggers
	// exactly one cadence request.
	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	Vp8MediaPacket third(3u, 97200u, false);
	CHECK(
	  producer.ReceiveRtpPacket(third.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 2u);

	// While waiting for the publisher's key frame, further packets must not
	// retrigger the request.
	Vp8MediaPacket fourth(4u, 100800u, false);
	CHECK(
	  producer.ReceiveRtpPacket(fourth.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(listener.sentRtcpPackets.size() == 2u);

	// A received key frame refreshes the cadence baseline.
	Vp8MediaPacket keyFrame(5u, 104400u, true);
	CHECK(
	  producer.ReceiveRtpPacket(keyFrame.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(listener.sentRtcpPackets.size() == 2u);

	// The watchdog only fires again after a full interval without a key frame.
	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	Vp8MediaPacket fifth(6u, 108000u, false);
	CHECK(
	  producer.ReceiveRtpPacket(fifth.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 3u);
}

TEST_CASE(
  "Producer cadence watchdog respects the last forwarded request time",
  "[producer][keyframe][cadence]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request =
	  BuildProduceRequest(builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true);
	RTC::Producer producer(&shared, "producer-cadence-spacing", &listener, request);

	// A key-frame first packet creates the stream without the new-stream
	// forced request: no request has been forwarded yet.
	Vp8MediaPacket keyFrame(1u, 90000u, true);
	CHECK(
	  producer.ReceiveRtpPacket(keyFrame.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 0u);

	// An internal request just before the cadence interval elapses is
	// forwarded immediately (no pending request, no open coalescing window).
	std::this_thread::sleep_for(std::chrono::milliseconds(350u));
	producer.RequestKeyFrame(MappedSsrc, /*fromViewerRtcp=*/false);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// At ~450ms the key-frame age exceeds the interval, but the last forwarded
	// request is only ~100ms old: the watchdog must NOT fire a second request.
	std::this_thread::sleep_for(std::chrono::milliseconds(100u));
	Vp8MediaPacket early(2u, 93600u, false);
	CHECK(
	  producer.ReceiveRtpPacket(early.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(listener.sentRtcpPackets.size() == 1u);

	// A packet burst while waiting must still not retrigger.
	Vp8MediaPacket burst(3u, 97200u, false);
	CHECK(
	  producer.ReceiveRtpPacket(burst.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(listener.sentRtcpPackets.size() == 1u);

	// Once both the key-frame age and the request spacing exceed the interval,
	// the watchdog fires exactly one more request.
	std::this_thread::sleep_for(std::chrono::milliseconds(400u));
	Vp8MediaPacket late(4u, 100800u, false);
	CHECK(
	  producer.ReceiveRtpPacket(late.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 2u);
}

TEST_CASE(
  "Producer coordinates NACK-generator key frame requests in cadence mode",
  "[producer][keyframe][cadence][nack]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request =
	  BuildProduceRequest(builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true);
	RTC::Producer producer(&shared, "producer-nack-coordination", &listener, request);

	// A key-frame first packet creates the stream (captured by the listener)
	// without any request.
	Vp8MediaPacket keyFrame(1u, 90000u, true);
	CHECK(
	  producer.ReceiveRtpPacket(keyFrame.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.sentRtcpPackets.size() == 0u);
	REQUIRE(listener.rtpStreams.size() == 1u);

	// First NACK-overflow notification in cadence mode goes through the
	// manager and is forwarded (no pending request, no open window).
	producer.OnRtpStreamKeyFrameRequired(listener.rtpStreams[0]);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// A second notification while the first request is still pending (no key
	// frame arrived) must be deduplicated instead of sending again.
	producer.OnRtpStreamKeyFrameRequired(listener.rtpStreams[0]);
	CHECK(listener.sentRtcpPackets.size() == 1u);

	// Legacy mode (delay 0) keeps the direct path: each notification forwards.
	flatbuffers::FlatBufferBuilder legacyBuilder;
	const auto* legacyRequest =
	  BuildProduceRequest(legacyBuilder, /*keyFrameRequestDelay=*/0u, /*withPliFeedback=*/true);
	RTC::Producer legacyProducer(&shared, "producer-nack-legacy", &listener, legacyRequest);

	Vp8MediaPacket legacyKeyFrame(1u, 90000u, true);
	CHECK(
	  legacyProducer.ReceiveRtpPacket(legacyKeyFrame.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.rtpStreams.size() == 2u);
	// Clear pending state from the cadence producer is irrelevant here; the
	// legacy producer has its own manager.
	const size_t before = listener.sentRtcpPackets.size();
	legacyProducer.OnRtpStreamKeyFrameRequired(listener.rtpStreams[1]);
	CHECK(listener.sentRtcpPackets.size() == before + 1u);
}


TEST_CASE(
  "Producer key frame tracker rejects a frame whose credible start was lost",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/VP9");
	RTC::Producer producer(&shared, "producer-keyframe-a1", &listener, request);

	// The frame really starts at seq 100, but 100/101 are lost.  Seq 102 is not
	// a credible frame start, even if later packets and the marker arrive.
	Vp9MediaPacket middle(102u, 90000u, false, false, false);
	CHECK(producer.ReceiveRtpPacket(middle.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket tail(103u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker requests recovery when the frame tail is lost",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/VP9");
	RTC::Producer producer(&shared, "producer-keyframe-a2", &listener, request);

	Vp9MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(producer.ReceiveRtpPacket(middle.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	// Seq 102 (E bit and marker) is lost.  The one-shot timer must classify the
	// candidate incomplete and schedule a bounded recovery request.
	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);
}

TEST_CASE(
  "Producer key frame tracker recovers packets that arrived before the frame start",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/VP9");
	RTC::Producer producer(&shared, "producer-keyframe-a3", &listener, request);

	// Seq 101 and the frame end at 102 arrive before the credible start at
	// seq 100.  The history must be merged into the candidate, including the
	// codec-parsed end and marker, instead of claiming missing packets.
	Vp9MediaPacket earlyMiddle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(earlyMiddle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket earlyTail(102u, 90000u, false, true, true);
	CHECK(
	  producer.ReceiveRtpPacket(earlyTail.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker merges RTX-recovered original sequence numbers",
  "[producer][keyframe][completeness][rtx]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder,
	  /*keyFrameRequestDelay=*/400u,
	  /*withPliFeedback=*/true,
	  "video/VP9",
	  /*withRtx=*/true);
	RTC::Producer producer(&shared, "producer-keyframe-rtx", &listener, request);

	Vp9MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	// Let the NACK generator emit the NACK for missing seq 101 (the stream uses
	// the production 10ms send delay).  A real RTX response arrives after that
	// request, so it is accepted as a recovered packet.
	std::this_thread::sleep_for(std::chrono::milliseconds(60u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	// Original seq 101 is lost, then recovered through RTX.  RtxDecode restores
	// the original sequence before Producer tracks it.
	Vp9MediaPacket lostMiddle(101u, 90000u, false, false, false);
	std::unique_ptr<RTC::RtpPacket> rtxPacket(lostMiddle.packet->Clone());
	REQUIRE(rtxPacket->RtxEncode(RtxPayloadType, RtxSsrc, 9000u));
	CHECK(
	  producer.ReceiveRtpPacket(rtxPacket.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::RETRANSMISSION);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker does not treat later key-frame slices as new frame heads",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/VP9");
	RTC::Producer producer(&shared, "producer-keyframe-a4", &listener, request);

	Vp9MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	// Another key-frame-marked slice in the SAME timestamp must not supersede
	// the original frame head.
	Vp9MediaPacket secondSlice(101u, 90000u, true, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(secondSlice.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker leaves a markerless VP8 frame unknown",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request =
	  BuildProduceRequest(builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true);
	RTC::Producer producer(&shared, "producer-keyframe-a5", &listener, request);

	// VP8 provides a codec start but no codec end.  Without the RTP marker the
	// frame end is unknown; it must not be guessed as complete.
	Vp8MediaPacket start(100u, 90000u, true, /*marker=*/false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 1u);
}

TEST_CASE(
  "Producer key frame tracker times out when media stops during a candidate",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/VP9");
	RTC::Producer producer(&shared, "producer-keyframe-a6", &listener, request);

	Vp9MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	// No later packet may drive the check: the one-shot timer alone must fire.
	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);
}


TEST_CASE(
  "Producer key frame tracker keeps an old candidate alive across a newer frame",
  "[producer][keyframe][completeness][rtx]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder,
	  /*keyFrameRequestDelay=*/400u,
	  /*withPliFeedback=*/true,
	  "video/VP9",
	  /*withRtx=*/true);
	RTC::Producer producer(&shared, "producer-keyframe-old-candidate-rtx", &listener, request);

	// Key frame at timestamp 90000 is missing seq 101.
	Vp9MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	REQUIRE(producer.testKeyFrameCandidateCount(ProducerSsrc) == 1u);

	// A newer non-key-frame timestamp arrives before RTX.  It must not finalize
	// or delete the old candidate.
	Vp9MediaPacket nextFrame(103u, 90360u, false, false, false, /*keyFrame=*/false);
	CHECK(
	  producer.ReceiveRtpPacket(nextFrame.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	REQUIRE(producer.testKeyFrameCandidateCount(ProducerSsrc) == 1u);

	// Let the NACK generator emit the NACK for seq 101, then repair the old
	// timestamp through RTX.  The old candidate must still accept it.
	std::this_thread::sleep_for(std::chrono::milliseconds(60u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	Vp9MediaPacket lostMiddle(101u, 90000u, false, false, false);
	std::unique_ptr<RTC::RtpPacket> rtxPacket(lostMiddle.packet->Clone());
	REQUIRE(rtxPacket->RtxEncode(RtxPayloadType, RtxSsrc, 9000u));
	CHECK(
	  producer.ReceiveRtpPacket(rtxPacket.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::RETRANSMISSION);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker does not request recovery for an old candidate after a newer key frame",
  "[producer][keyframe][completeness]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/VP9");
	RTC::Producer producer(&shared, "producer-keyframe-old-timeout", &listener, request);

	// Timestamp 90000 is incomplete: seq 101 is missing.
	Vp9MediaPacket oldStart(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(oldStart.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket oldTail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(oldTail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testKeyFrameCandidateCount(ProducerSsrc) == 1u);

	// A newer key frame completes.  It clears pending state and becomes the
	// latest started candidate, while the old candidate remains for RTX repair.
	Vp9MediaPacket newStart(103u, 90360u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(newStart.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket newMiddle(104u, 90360u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(newMiddle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	Vp9MediaPacket newTail(105u, 90360u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(newTail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	REQUIRE(producer.testKeyFrameCandidateCount(ProducerSsrc) == 1u);
	REQUIRE(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(listener.sentRtcpPackets.empty());

	// The old candidate may still time out.  It must record incomplete evidence
	// but must not issue another publisher key-frame request.
	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(listener.sentRtcpPackets.empty());
}

TEST_CASE(
  "Producer key frame tracker does not infer an H265 frame start from a FU start",
  "[producer][keyframe][completeness][h265]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H265");
	RTC::Producer producer(&shared, "producer-keyframe-h265-fu", &listener, request);

	H265MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(start.packet->IsKeyFrame());
	// FU S is a NAL-fragment boundary.  Without frame marking it cannot prove
	// that an earlier slice of the same access unit was received.
	REQUIRE_FALSE(start.packet->IsFrameStart());
	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	H265MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker does not infer an H264 frame start from a FU start",
  "[producer][keyframe][completeness][h264]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-h264-fu", &listener, request);

	H264MediaPacket start(100u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(start.packet->IsKeyFrame());
	REQUIRE_FALSE(start.packet->IsFrameStart());
	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	H264MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}


TEST_CASE(
  "Producer key frame tracker uses H265 first_slice_segment_in_pic_flag boundaries",
  "[producer][keyframe][completeness][h265][first-slice]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H265");
	RTC::Producer producer(&shared, "producer-keyframe-h265-first-slice", &listener, request);

	H265MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstSlice=*/true);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(start.packet->IsKeyFrame());
	REQUIRE(start.packet->IsFrameStart());
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	H265MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker rejects a multi-slice H265 key frame whose first slice was lost",
  "[producer][keyframe][completeness][h265][first-slice]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H265");
	RTC::Producer producer(&shared, "producer-keyframe-h265-slice2-first", &listener, request);

	// Slice 1 (the picture start with first_slice_segment_in_pic_flag=1) is
	// lost.  Slice 2's FU start fragment must not be mistaken for a frame
	// start: its slice header says first_slice=0.
	H265MediaPacket slice2Start(101u, 90000u, true, false, false, /*frameMarking=*/false, /*firstSlice=*/false);
	CHECK(
	  producer.ReceiveRtpPacket(slice2Start.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(slice2Start.packet->IsKeyFrame());
	REQUIRE_FALSE(slice2Start.packet->IsFrameStart());

	H265MediaPacket middle(102u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket tail(103u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker completes a single-NAL H265 key frame via the RTP marker",
  "[producer][keyframe][completeness][h265][single-nal]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H265");
	RTC::Producer producer(&shared, "producer-keyframe-h265-single-nal", &listener, request);

	// A whole IDR NAL in one RTP packet: 2-byte NAL header (IDR_W_RADL=19)
	// followed by a slice segment header with first_slice=1.
	std::array<uint8_t, 1600u> buffer{};
	buffer[0]  = 0x80u;
	buffer[1]  = PayloadType;
	buffer[12] = 19u << 1;
	buffer[13] = 0x01u;
	buffer[14] = 0x80u;
	auto packet = std::unique_ptr<RTC::RtpPacket>(
	  RTC::RtpPacket::Parse(buffer.data(), RTC::RtpPacket::HeaderSize + 3u, buffer.size()));
	REQUIRE(packet);
	packet->SetPayloadType(PayloadType);
	packet->SetSequenceNumber(100u);
	packet->SetTimestamp(90000u);
	packet->SetSsrc(ProducerSsrc);
	packet->SetMarker(true);

	CHECK(producer.ReceiveRtpPacket(packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(packet->IsKeyFrame());
	REQUIRE(packet->IsFrameStart());
	REQUIRE(packet->IsFrameEnd(true));

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker uses H264 first_mb_in_slice boundaries",
  "[producer][keyframe][completeness][h264][first-slice]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-h264-first-slice", &listener, request);

	H264MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(start.packet->IsKeyFrame());
	REQUIRE(start.packet->IsFrameStart());
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	H264MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker rejects a multi-slice H264 key frame whose first slice was lost",
  "[producer][keyframe][completeness][h264][first-slice]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-h264-slice2-first", &listener, request);

	// First slice (first_mb_in_slice == 0) is lost.  Slice 2's FU start with
	// a non-zero first_mb must not be mistaken for a frame start.
	H264MediaPacket slice2Start(101u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/false);
	CHECK(
	  producer.ReceiveRtpPacket(slice2Start.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(slice2Start.packet->IsKeyFrame());
	REQUIRE_FALSE(slice2Start.packet->IsFrameStart());

	H264MediaPacket middle(102u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket tail(103u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker disables the first-slice heuristic on an H264 FMO-like conflict",
  "[producer][keyframe][completeness][h264][first-slice][fmo]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-h264-fmo-conflict", &listener, request);

	// Slice group 0 first slice.
	H264MediaPacket firstStart(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(firstStart.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	// A second, different first_mb==0 start within the same picture is FMO
	// evidence: the heuristic must be disabled and candidates dropped instead
	// of risking a false complete.
	H264MediaPacket secondStart(103u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(secondStart.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK(producer.testKeyFrameStartHeuristicDisabled(ProducerSsrc));
	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);

	// Even a fully received later key frame must not start a candidate once
	// the heuristic is disabled for this SSRC.
	H264MediaPacket nextStart(200u, 91000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(nextStart.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
}

TEST_CASE(
  "Producer key frame integrity observe mode preserves legacy pending clearing",
  "[producer][keyframe][completeness][observe]")
{
	REQUIRE(setenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE", "observe", 1) == 0);

	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	// Legacy mode (delay 0) plus observation.  The regression this guards:
	// observation must not displace the legacy per-key-frame receipt handling
	// that clears the request manager's pending state.  Otherwise a forced
	// new-stream request would be re-sent after the 1s retry timer even
	// though the key frame arrived.
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/0u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-observe-legacy", &listener, request);
	REQUIRE(producer.testKeyFrameIntegrityObserve());

	// First packet on a new stream is a delta frame: the producer forces a
	// key frame request (one PLI, pending state created).
	H264MediaPacket deltaFirst(1u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/false, /*nalType=*/1u);
	CHECK(
	  producer.ReceiveRtpPacket(deltaFirst.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(listener.rtpStreams.size() == 1u);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	// A complete key frame arrives.  The legacy path must clear the pending
	// request, and observation must record the complete frame.
	H264MediaPacket idrStart(2u, 94500u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(idrStart.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket idrTail(3u, 94500u, false, true, true);
	CHECK(
	  producer.ReceiveRtpPacket(idrTail.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);

	// Wait past the 1s pending retry timer: no re-request may be sent.
	std::this_thread::sleep_for(std::chrono::milliseconds(1200u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK(listener.sentRtcpPackets.size() == 1u);

	unsetenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE");
}

TEST_CASE(
  "Producer key frame integrity observe mode preserves cadence recovery requests",
  "[producer][keyframe][completeness][observe]")
{
	REQUIRE(setenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE", "observe", 1) == 0);

	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	// Cadence mode (delay > 0) plus observation: scheduling behavior must be
	// identical to cadence mode without observation.  Observation only adds
	// evidence; it must neither suppress nor add requests.
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H265");
	RTC::Producer producer(&shared, "producer-keyframe-observe-cadence", &listener, request);
	REQUIRE(producer.testKeyFrameIntegrityObserve());

	// Complete key frame: evidence is counted.
	H265MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstSlice=*/true);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket tail(101u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(listener.sentRtcpPackets.empty());

	// Incomplete key frame (tail lost): the timeout classifies it incomplete
	// and cadence-mode recovery still requests exactly once, as without
	// observation.
	H265MediaPacket lostStart(200u, 91000u, true, false, false, /*frameMarking=*/false, /*firstSlice=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(lostStart.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket lostMiddle(201u, 91000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(lostMiddle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(listener.sentRtcpPackets.size() == 1u);

	unsetenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE");
}

TEST_CASE(
  "Producer key frame tracker warns when a FU-fragmented key frame loses its first slice",
  "[producer][keyframe][completeness][h264][first-slice][no-start]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-h264-no-start", &listener, request);

	// Slice 1 (first_mb == 0) is lost.  Slice 2 arrives as a FU start (key
	// frame NAL traffic, no marker) and the frame ends on a marker packet
	// that itself is NOT key-frame marked.
	H264MediaPacket slice2Start(101u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/false);
	CHECK(
	  producer.ReceiveRtpPacket(slice2Start.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(slice2Start.packet->IsKeyFrame());
	REQUIRE_FALSE(slice2Start.packet->HasMarker());

	H264MediaPacket tail(103u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(tail.packet->HasMarker());
	REQUIRE_FALSE(tail.packet->IsKeyFrame());

	// No candidate may start, and the per-timestamp key frame traffic must
	// make the missing start observable.
	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testSawKeyFrameTraffic(ProducerSsrc, 90000u));
	CHECK(producer.testKeyFrameNoStartWarned(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker warns when a key frame FU start fragment is lost",
  "[producer][keyframe][completeness][h264][first-slice][no-start][fu-start-lost]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-h264-fu-start-lost", &listener, request);

	// The FU start fragment (and with it the first slice) is lost.  Only
	// middle and tail fragments arrive: none of them is IsKeyFrame(), but the
	// FU header repeats the IDR NAL type on every fragment, so key frame
	// traffic remains observable.
	H264MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE_FALSE(middle.packet->IsKeyFrame());
	REQUIRE(middle.packet->IsKeyFrameNal());

	H264MediaPacket tail(102u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(tail.packet->HasMarker());
	REQUIRE_FALSE(tail.packet->IsKeyFrame());
	REQUIRE(tail.packet->IsKeyFrameNal());

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testSawKeyFrameTraffic(ProducerSsrc, 90000u));
	CHECK(producer.testKeyFrameNoStartWarned(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame evidence timer flushes the latest state after a stream stops sending",
  "[producer][keyframe][completeness][evidence][stop-flush]")
{
	REQUIRE(setenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE", "observe", 1) == 0);

	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	// Single-stream video in legacy mode: there is no RTP-inactivity score
	// event, so only the evidence timer can flush after the stream stops.
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/0u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer(&shared, "producer-keyframe-stop-flush", &listener, request);
	REQUIRE(producer.testKeyFrameIntegrityObserve());
	producer.testSetKeyFrameEvidenceFlushIntervalMs(300u);

	// First complete key frame emits the initial summary (event path).
	H264MediaPacket first1(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(first1.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket first2(101u, 90000u, false, true, true);
	CHECK(
	  producer.ReceiveRtpPacket(first2.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(producer.testKeyFrameSummaryEmissionCount() == 1u);
	REQUIRE(producer.testKeyFrameEvidenceTimerActive());

	// Second complete key frame arrives within the summary rate window: no
	// event summary is emitted, but the state is now dirty.
	H264MediaPacket second1(200u, 94500u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(second1.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket second2(201u, 94500u, false, true, true);
	CHECK(
	  producer.ReceiveRtpPacket(second2.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 2u);
	CHECK(producer.testKeyFrameSummaryEmissionCount() == 1u);

	// The stream stops sending.  The producer stays alive; only the evidence
	// timer may flush the latest state.
	std::this_thread::sleep_for(std::chrono::milliseconds(450u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	REQUIRE(producer.testKeyFrameSummaryEmissionCount() == 2u);
	CHECK(producer.testLastKeyFrameCompleteAtMs(ProducerSsrc) != 0u);
	// Nothing dirty remains: the timer stops until new evidence arrives.
	CHECK_FALSE(producer.testKeyFrameEvidenceTimerActive());

	unsetenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE");
}

TEST_CASE(
  "Producer key frame evidence timer must not restart during destruction",
  "[producer][keyframe][completeness][evidence][destroy]")
{
	REQUIRE(setenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE", "observe", 1) == 0);

	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;

	// Inner scope: the producer receives a key frame head without its tail and
	// is then destroyed with an active candidate.  Destruction finalizes the
	// candidate as incomplete; the evidence timer must not be restarted for
	// the dying object, otherwise its callback fires after free once the loop
	// keeps running below.
	{
		const auto* request = BuildProduceRequest(
		  builder, /*keyFrameRequestDelay=*/0u, /*withPliFeedback=*/true, "video/H264");
		//Constructed in-place via unique scope below.
		auto producer = std::make_unique<RTC::Producer>(
		  &shared, "producer-keyframe-destroy-timer", &listener, request);
		REQUIRE(producer->testKeyFrameIntegrityObserve());
		producer->testSetKeyFrameEvidenceFlushIntervalMs(100u);

		H264MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
		CHECK(
		  producer->ReceiveRtpPacket(start.packet.get()) ==
		  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
		REQUIRE(producer->testHasKeyFrameCandidate(ProducerSsrc));

		producer.reset();
	}

	// The producer is gone but the event loop keeps running: any timer that
	// was (incorrectly) restarted during destruction fires here.  Under
	// ASan/UBSan this reproduces the use-after-free; in release builds it is
	// a smoke check.
	std::this_thread::sleep_for(std::chrono::milliseconds(350u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	unsetenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE");
}

TEST_CASE(
  "Producer no-start warning classifies by phase and escalates repeated pre-complete events",
  "[producer][keyframe][completeness][no-start][classification]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	// Short no-start rate limit so the escalation case is testable.
	RTC::Producer producer(&shared, "producer-keyframe-no-start-class", &listener, request);
	producer.testSetKeyFrameNoStartWarnIntervalMs(20u);

	// Case A: a no-start before any complete key frame is phase
	// pre-first-complete and logged at INFO.
	H264MediaPacket a1(101u, 90000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(a1.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket a2(103u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(a2.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testKeyFrameNoStartWarned(ProducerSsrc));
	REQUIRE(producer.testKeyFrameNoStartWarningCount(ProducerSsrc) == 1u);
	REQUIRE(producer.testLastKeyFrameNoStartInfoClassified(ProducerSsrc));

	// Case B (review counter-example): after a complete key frame, a no-start
	// is WARN regardless of elapsed time — real loss on an established stream
	// must not be downgraded.
	std::this_thread::sleep_for(std::chrono::milliseconds(30u));
	H264MediaPacket b1(200u, 180000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(producer.ReceiveRtpPacket(b1.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket b2(201u, 180000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(b2.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);

	std::this_thread::sleep_for(std::chrono::milliseconds(30u));
	H264MediaPacket c1(301u, 270000u, true, false, false);
	CHECK(producer.ReceiveRtpPacket(c1.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket c2(303u, 270000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(c2.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer.testKeyFrameNoStartWarningCount(ProducerSsrc) == 2u);
	CHECK_FALSE(producer.testLastKeyFrameNoStartInfoClassified(ProducerSsrc));

	// Case C: repeated pre-first-complete no-starts escalate to WARN.  Use a
	// fresh producer because the one above already completed a key frame.
	flatbuffers::FlatBufferBuilder builder2;
	const auto* request2 = BuildProduceRequest(
	  builder2, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer2(&shared, "producer-keyframe-no-start-escalate", &listener, request2);
	producer2.testSetKeyFrameNoStartWarnIntervalMs(20u);

	for (uint32_t ts = 90000u; ts <= 270000u; ts += 90000u)
	{
		H264MediaPacket start(ts == 90000u ? 100u : static_cast<uint16_t>(100u + (ts - 90000u) / 1000u * 10u), ts, true, false, false);
		CHECK(producer2.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
		H264MediaPacket tail(static_cast<uint16_t>(start.packet->GetSequenceNumber() + 2u), ts, false, true, true);
		CHECK(producer2.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
		std::this_thread::sleep_for(std::chrono::milliseconds(30u));
	}

	REQUIRE(producer2.testKeyFrameNoStartWarningCount(ProducerSsrc) >= 3u);
	CHECK_FALSE(producer2.testLastKeyFrameNoStartInfoClassified(ProducerSsrc));

	// Case D: a stream rebuild resets the per-generation phase.  The same
	// SSRC completes key frames, is rebuilt, and its first no-start must
	// classify pre-first-complete again (INFO) even though the lifetime
	// complete counter is non-zero.
	flatbuffers::FlatBufferBuilder builder3;
	const auto* request3 = BuildProduceRequest(
	  builder3, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H264");
	RTC::Producer producer3(&shared, "producer-keyframe-no-start-rebuild", &listener, request3);
	producer3.testSetKeyFrameNoStartWarnIntervalMs(20u);

	H264MediaPacket d1(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstMb0=*/true);
	CHECK(producer3.ReceiveRtpPacket(d1.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket d2(101u, 90000u, false, true, true);
	CHECK(producer3.ReceiveRtpPacket(d2.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer3.testCompleteKeyFrameCount(ProducerSsrc) == 1u);

	// Stream rebuild: same SSRC, new generation.  Uses the same reset the
	// rebuild path calls in production code.
	producer3.testResetKeyFrameGenerationState(ProducerSsrc);

	// Lifetime counter persists; generation flag is cleared.
	REQUIRE(producer3.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(producer3.testKeyFrameNoStartWarningCount(ProducerSsrc) == 0u);

	// First no-start of the new generation classifies pre-first-complete
	// (INFO) despite the non-zero lifetime counter.
	std::this_thread::sleep_for(std::chrono::milliseconds(30u));
	H264MediaPacket e1(300u, 270000u, true, false, false);
	CHECK(producer3.ReceiveRtpPacket(e1.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket e2(302u, 270000u, false, true, true);
	CHECK(producer3.ReceiveRtpPacket(e2.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(producer3.testKeyFrameNoStartWarningCount(ProducerSsrc) == 1u);
	REQUIRE(producer3.testLastKeyFrameNoStartInfoClassified(ProducerSsrc));

}

TEST_CASE(
  "Producer key frame evidence tracks the last complete and incomplete times",
  "[producer][keyframe][completeness][evidence]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder, /*keyFrameRequestDelay=*/400u, /*withPliFeedback=*/true, "video/H265");
	RTC::Producer producer(&shared, "producer-keyframe-evidence-times", &listener, request);

	REQUIRE(producer.testLastKeyFrameCompleteAtMs(ProducerSsrc) == 0u);
	REQUIRE(producer.testLastKeyFrameIncompleteAtMs(ProducerSsrc) == 0u);

	H265MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/false, /*firstSlice=*/true);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket tail(101u, 90000u, false, true, true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(producer.testLastKeyFrameCompleteAtMs(ProducerSsrc) != 0u);

	// An incomplete frame (tail lost) records the incomplete time.
	H265MediaPacket lostStart(200u, 91000u, true, false, false, /*frameMarking=*/false, /*firstSlice=*/true);
	CHECK(
	  producer.ReceiveRtpPacket(lostStart.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	std::this_thread::sleep_for(std::chrono::milliseconds(550u));
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);
	uv_run(DepLibUV::GetLoop(), UV_RUN_NOWAIT);

	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 1u);
	REQUIRE(producer.testLastKeyFrameIncompleteAtMs(ProducerSsrc) != 0u);
	REQUIRE(
	  producer.testLastKeyFrameIncompleteAtMs(ProducerSsrc) >=
	  producer.testLastKeyFrameCompleteAtMs(ProducerSsrc));
}

TEST_CASE(
  "Producer key frame tracker uses H264 frame marking boundaries",
  "[producer][keyframe][completeness][h264][frame-marking]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder,
	  /*keyFrameRequestDelay=*/400u,
	  /*withPliFeedback=*/true,
	  "video/H264",
	  /*withRtx=*/false,
	  /*withFrameMarking=*/true);
	RTC::Producer producer(&shared, "producer-keyframe-h264-frame-marking", &listener, request);

	H264MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/true);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(start.packet->IsKeyFrame());
	REQUIRE(start.packet->IsFrameStart());
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	H264MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H264MediaPacket tail(102u, 90000u, false, true, true, /*frameMarking=*/true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

TEST_CASE(
  "Producer key frame tracker uses H265 frame marking boundaries",
  "[producer][keyframe][completeness][h265][frame-marking]")
{
	Channel::ChannelSocket channel(NoChannelMessage, nullptr, IgnoreChannelWrite, nullptr);
	RTC::Shared shared(new ChannelMessageRegistrator(), new Channel::ChannelNotifier(&channel));
	TestProducerListener listener;
	flatbuffers::FlatBufferBuilder builder;
	const auto* request = BuildProduceRequest(
	  builder,
	  /*keyFrameRequestDelay=*/400u,
	  /*withPliFeedback=*/true,
	  "video/H265",
	  /*withRtx=*/false,
	  /*withFrameMarking=*/true);
	RTC::Producer producer(&shared, "producer-keyframe-h265-frame-marking", &listener, request);

	H265MediaPacket start(100u, 90000u, true, false, false, /*frameMarking=*/true);
	CHECK(producer.ReceiveRtpPacket(start.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	REQUIRE(start.packet->IsKeyFrame());
	REQUIRE(start.packet->IsFrameStart());
	REQUIRE(producer.testHasKeyFrameCandidate(ProducerSsrc));

	H265MediaPacket middle(101u, 90000u, false, false, false);
	CHECK(
	  producer.ReceiveRtpPacket(middle.packet.get()) ==
	  RTC::Producer::ReceiveRtpPacketResult::MEDIA);
	H265MediaPacket tail(102u, 90000u, false, true, true, /*frameMarking=*/true);
	CHECK(producer.ReceiveRtpPacket(tail.packet.get()) == RTC::Producer::ReceiveRtpPacketResult::MEDIA);

	CHECK_FALSE(producer.testHasKeyFrameCandidate(ProducerSsrc));
	CHECK(producer.testCompleteKeyFrameCount(ProducerSsrc) == 1u);
	CHECK(producer.testIncompleteKeyFrameCount(ProducerSsrc) == 0u);
}

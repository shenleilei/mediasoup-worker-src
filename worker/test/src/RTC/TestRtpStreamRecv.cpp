#include "common.hpp"
#include "DepLibUV.hpp"
#include "FBS/rtpStream.h"
#include "RTC/RTCP/SenderReport.hpp"
#include "RTC/RTCP/XrDelaySinceLastRr.hpp"
#include "RTC/RtpPacket.hpp"
#include "RTC/RtpStream.hpp"
#include "RTC/RtpStreamRecv.hpp"
#include "Utils.hpp"
#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <utility>
#include <vector>

using namespace RTC;

// 17: 16 bit mask + the initial sequence number.
static constexpr size_t MaxRequestedPackets{ 17 };
static constexpr unsigned int SendNackDelay{ 0u }; // In ms.
static const bool UseRtpInactivityCheck{ false };

TEST_CASE("instant producer score mapping", "[rtp][rtpstream][score]")
{
	SECTION("loss mapping")
	{
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 0) == 10u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 4) == 8u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 8) == 5u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 9) == 5u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 10) == 4u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 12) == 3u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 16) == 2u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 20) == 1u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 24) == 0u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(100, 40) == 0u);
		REQUIRE(RtpStreamRecv::ComputeInstantLossScore(0, 0) == 10u);
	}

	SECTION("rtt mapping")
	{
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(0.0f) == 10u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(100.0f) == 10u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(200.0f) == 8u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(300.0f) == 6u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(400.0f) == 4u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(500.0f) == 3u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(800.0f) == 2u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(1000.0f) == 1u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(1500.0f) == 0u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(2000.0f) == 0u);
		REQUIRE(RtpStreamRecv::ComputeInstantRttScore(5000.0f) == 0u);
	}

	SECTION("combined mapping takes the worst signal")
	{
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 0, 50.0f) == 10u);
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 0, 500.0f) == 3u);
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 8, 100.0f) == 5u);
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 0, 400.0f) == 4u);
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 20, 100.0f) == 1u);
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 20, 500.0f) == 1u);
		REQUIRE(RtpStreamRecv::ComputeInstantScore(100, 40, 500.0f) == 0u);
	}
}

SCENARIO("XR RTT updates instant score without waiting for SR", "[rtp][rtpstream][score]")
{
	class RtpStreamRecvListener : public RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
			++this->scoreEventCount;
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* /*rtpStream*/, RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  bool /*rtpActive*/,
		  uint64_t /*transitionAtMs*/,
		  uint64_t /*workerEventAtMs*/,
		  uint64_t /*lastRtpActivityAtMs*/,
		  uint32_t /*rtpActivityThresholdMs*/,
		  uint64_t /*rtpActivityStateVersion*/) override
		{
		}

	public:
		size_t scoreEventCount{ 0u };
	};

	RtpStream::Params params;

	params.ssrc = 1234u;
	params.clockRate = 90000;
	params.mimeType.type = RTC::RtpCodecMimeType::Type::VIDEO;
	params.mimeType.subtype = RTC::RtpCodecMimeType::Subtype::VP8;
	params.mimeType.UpdateMimeType();

	RtpStreamRecvListener listener;
	RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

	REQUIRE(rtpStream.GetScore() == 10u);
	REQUIRE(rtpStream.GetInstantScore() == 10u);

	const auto ntp = Utils::Time::TimeMs2Ntp(DepLibUV::GetTimeMs());
	uint32_t compactNtp = (ntp.seconds & 0x0000FFFF) << 16;
	compactNtp |= (ntp.fractions & 0xFFFF0000) >> 16;

	// 32768 compact NTP units are 500ms. Reserve one unit for DLRR so the
	// resulting RTT is exactly 32768 units.
	auto* ssrcInfo = new RTCP::DelaySinceLastRr::SsrcInfo();
	ssrcInfo->SetSsrc(params.ssrc);
	ssrcInfo->SetLastReceiverReport(compactNtp - 32769u);
	ssrcInfo->SetDelaySinceLastReceiverReport(1u);

	rtpStream.testReceiveRtcpXrDelaySinceLastRr(ssrcInfo);

	REQUIRE(rtpStream.GetInstantScore() == 3u);
	REQUIRE(listener.scoreEventCount == 1u);
	REQUIRE(rtpStream.GetInstantLossRatio() == 0.0f);
	REQUIRE(rtpStream.GetInstantRttMs() == 500.0f);

	flatbuffers::FlatBufferBuilder statsBuilder;
	auto statsOffset = rtpStream.FillBufferStats(statsBuilder);
	statsBuilder.Finish(statsOffset);

	const auto* stats = flatbuffers::GetRoot<FBS::RtpStream::Stats>(statsBuilder.GetBufferPointer());
	REQUIRE(stats);
	const auto* recvStats = stats->data_as_RecvStats();
	REQUIRE(recvStats);
	REQUIRE(recvStats->base());
	const auto* baseStats = recvStats->base()->data_as_BaseStats();
	REQUIRE(baseStats);
	REQUIRE(baseStats->instantScore() == 3u);
	REQUIRE(baseStats->instantLossRatio() == 0.0f);
	REQUIRE(baseStats->instantRttMs() == 500.0f);

	delete ssrcInfo;
}

SCENARIO("loss-only instant score drop notifies without legacy score change", "[rtp][rtpstream][score]")
{
	class CountingListener : public RtpStreamRecv::Listener
	{
	public:
		size_t scoreEventCount{ 0u };

		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
			this->scoreEventCount++;
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* /*rtpStream*/, RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RtpStreamRecv* /*rtpStream*/,
		  bool /*rtpActive*/,
		  uint64_t /*transitionAtMs*/,
		  uint64_t /*workerEventAtMs*/,
		  uint64_t /*lastRtpActivityAtMs*/,
		  uint32_t /*rtpActivityThresholdMs*/,
		  uint64_t /*rtpActivityStateVersion*/) override
		{
		}
	};

	// clang-format off
	uint8_t buffer[] =
	{
		0x80, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x05
	};
	// clang-format on

	RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

	REQUIRE(packet);

	RtpStream::Params params;

	params.ssrc      = packet->GetSsrc();
	params.clockRate = 90000;
	params.mimeType.type = RTC::RtpCodecMimeType::Type::VIDEO;
	params.mimeType.subtype = RTC::RtpCodecMimeType::Subtype::VP8;
	params.mimeType.UpdateMimeType();

	CountingListener listener;
	RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

	// Activate the stream and fill the 24-entry score histogram with perfect
	// windows so the smoothed legacy score is pinned at 10.
	packet->SetSequenceNumber(1);
	REQUIRE(rtpStream.ReceivePacket(packet) == true);

	RTCP::SenderReport senderReport;
	senderReport.SetSsrc(params.ssrc);

	for (uint8_t i = 0; i < 30; ++i)
	{
		rtpStream.ReceiveRtcpSenderReport(&senderReport);
	}

	REQUIRE(rtpStream.GetScore() == 10u);
	REQUIRE(rtpStream.GetInstantScore() == 10u);

	const auto eventsBefore = listener.scoreEventCount;

	// Next window: expected 29, received 25, lost 4 (loss ratio ~13.8%).
	// The legacy sample is round(((25-4)/25)^4 * 10) = 5, and the weighted
	// average of [10 x 23, 5] stays at 10, so the legacy score does not move.
	// The instant loss score for 13.8% is 3, so instantScore must drop to 3
	// and MUST emit a score event even though the legacy score is unchanged.
	uint16_t seq = 2;

	for (uint16_t slot = 0; slot < 29; ++slot)
	{
		if (slot == 5 || slot == 12 || slot == 19 || slot == 26)
		{
			seq++;

			continue;
		}

		packet->SetSequenceNumber(seq++);
		REQUIRE(rtpStream.ReceivePacket(packet) == true);
	}

	rtpStream.ReceiveRtcpSenderReport(&senderReport);

	REQUIRE(rtpStream.GetScore() == 10u);
	REQUIRE(rtpStream.GetInstantScore() == 3u);
	REQUIRE(listener.scoreEventCount > eventsBefore);
	REQUIRE(rtpStream.GetInstantLossRatio() > 0.13f);
	REQUIRE(rtpStream.GetInstantLossRatio() < 0.14f);
}

SCENARIO("receive RTP packets and trigger NACK", "[rtp][rtpstream]")
{
	class RtpStreamRecvListener : public RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* rtpStream, RTCP::Packet* packet) override
		{
			switch (packet->GetType())
			{
				case RTCP::Type::PSFB:
				{
					switch (dynamic_cast<RTCP::FeedbackPsPacket*>(packet)->GetMessageType())
					{
						case RTCP::FeedbackPs::MessageType::PLI:
						{
							INFO("PLI required");

							REQUIRE(this->shouldTriggerPLI == true);

							this->shouldTriggerPLI = false;
							this->nackedSeqNumbers.clear();

							break;
						}

						case RTCP::FeedbackPs::MessageType::FIR:
						{
							INFO("FIR required");

							REQUIRE(this->shouldTriggerFIR == true);

							this->shouldTriggerFIR = false;
							this->nackedSeqNumbers.clear();

							break;
						}

						default:;
					}

					break;
				}

				case RTCP::Type::RTPFB:
				{
					switch (dynamic_cast<RTCP::FeedbackRtpPacket*>(packet)->GetMessageType())
					{
						case RTCP::FeedbackRtp::MessageType::NACK:
						{
							INFO("NACK required");

							REQUIRE(this->shouldTriggerNack == true);

							this->shouldTriggerNack = false;

							auto* nackPacket = dynamic_cast<RTCP::FeedbackRtpNackPacket*>(packet);

							for (auto it = nackPacket->Begin(); it != nackPacket->End(); ++it)
							{
								RTCP::FeedbackRtpNackItem* item = *it;

								uint16_t firstSeq = item->GetPacketId();
								uint16_t bitmask  = item->GetLostPacketBitmask();

								this->nackedSeqNumbers.push_back(firstSeq);

								for (size_t i{ 1 }; i < MaxRequestedPackets; ++i)
								{
									if ((bitmask & 1) != 0)
									{
										this->nackedSeqNumbers.push_back(firstSeq + i);
									}

									bitmask >>= 1;
								}
							}

							break;
						}

						default:;
					}

					break;
				}

				default:;
			}
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  bool /*rtpActive*/,
		  uint64_t /*transitionAtMs*/,
		  uint64_t /*workerEventAtMs*/,
		  uint64_t /*lastRtpActivityAtMs*/,
		  uint32_t /*rtpActivityThresholdMs*/,
		  uint64_t /*rtpActivityStateVersion*/) override
		{
		}

	public:
		bool shouldTriggerNack = false;
		bool shouldTriggerPLI  = false;
		bool shouldTriggerFIR  = false;
		std::vector<uint16_t> nackedSeqNumbers;
	};

	// clang-format off
	uint8_t buffer[] =
	{
		0x80, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x05
	};
	// clang-format on

	RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

	if (!packet)
	{
		FAIL("not a RTP packet");
	}

	RtpStream::Params params;

	params.ssrc      = packet->GetSsrc();
	params.clockRate = 90000;
	params.useNack   = true;
	params.usePli    = true;
	params.useFir    = false;

	SECTION("NACK one packet")
	{
		RtpStreamRecvListener listener;
		RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

		packet->SetSequenceNumber(1);
		rtpStream.ReceivePacket(packet);

		packet->SetSequenceNumber(3);
		listener.shouldTriggerNack = true;
		listener.shouldTriggerPLI  = false;
		listener.shouldTriggerFIR  = false;
		rtpStream.ReceivePacket(packet);

		REQUIRE(listener.nackedSeqNumbers.size() == 1);
		REQUIRE(listener.nackedSeqNumbers[0] == 2);
		REQUIRE(rtpStream.GetNewlyMissingPackets() == 1u);
		REQUIRE(rtpStream.GetRetransmittedPackets() == 0u);
		REQUIRE(rtpStream.GetRepairedPackets() == 0u);
		REQUIRE(rtpStream.GetUnrecoveredPackets() == 0u);
		listener.nackedSeqNumbers.clear();

		packet->SetSequenceNumber(2);
		rtpStream.ReceivePacket(packet);

		REQUIRE(listener.nackedSeqNumbers.size() == 0);
		REQUIRE(rtpStream.GetNewlyMissingPackets() == 1u);
		REQUIRE(rtpStream.GetRetransmittedPackets() == 1u);
		REQUIRE(rtpStream.GetRepairedPackets() == 1u);
		REQUIRE(rtpStream.GetUnrecoveredPackets() == 0u);

		packet->SetSequenceNumber(4);
		rtpStream.ReceivePacket(packet);

		REQUIRE(listener.nackedSeqNumbers.size() == 0);
	}

	SECTION("wrapping sequence numbers")
	{
		RtpStreamRecvListener listener;
		RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

		packet->SetSequenceNumber(0xfffe);
		rtpStream.ReceivePacket(packet);

		packet->SetSequenceNumber(1);
		listener.shouldTriggerNack = true;
		listener.shouldTriggerPLI  = false;
		listener.shouldTriggerFIR  = false;
		rtpStream.ReceivePacket(packet);

		REQUIRE(listener.nackedSeqNumbers.size() == 2);
		REQUIRE(listener.nackedSeqNumbers[0] == 0xffff);
		REQUIRE(listener.nackedSeqNumbers[1] == 0);
		REQUIRE(rtpStream.GetNewlyMissingPackets() == 2u);
		listener.nackedSeqNumbers.clear();
	}

	SECTION("require key frame")
	{
		RtpStreamRecvListener listener;
		RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

		packet->SetSequenceNumber(1);
		rtpStream.ReceivePacket(packet);

		// Seq different is bigger than MaxNackPackets in NackGenerator, so it
		// triggers a key frame.
		packet->SetSequenceNumber(1003);
		listener.shouldTriggerPLI = true;
		listener.shouldTriggerFIR = false;
		rtpStream.ReceivePacket(packet);
		REQUIRE(rtpStream.GetNewlyMissingPackets() == 1001u);
		REQUIRE(rtpStream.GetUnrecoveredPackets() == 1001u);
	}

	// Must run the loop to wait for UV timers and close them.
	DepLibUV::RunLoop();

	delete packet;
}

SCENARIO("receive RTP packet with abs-capture-time and expose it in stats", "[rtp][rtpstream][stats]")
{
	class RtpStreamRecvListener : public RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* /*rtpStream*/, RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  bool /*rtpActive*/,
		  uint64_t /*transitionAtMs*/,
		  uint64_t /*workerEventAtMs*/,
		  uint64_t /*lastRtpActivityAtMs*/,
		  uint32_t /*rtpActivityThresholdMs*/,
		  uint64_t /*rtpActivityStateVersion*/) override
		{
		}
	};

	// clang-format off
	uint8_t buffer[] =
	{
		0x90, 0x01, 0x00, 0x08,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x05,
		0x10, 0x00, 0x00, 0x05, // Header Extension (two-byte, 20 bytes)
		0x0d, 0x10,             // id=13 len=16
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8,
		0x00, 0x00
	};
	// clang-format on

	RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

	if (!packet)
	{
		FAIL("not a RTP packet");
	}

	packet->SetAbsCaptureTimeExtensionId(13);

	RtpStream::Params params;

	params.ssrc        = packet->GetSsrc();
	params.payloadType = packet->GetPayloadType();
	params.clockRate   = 90000;
	params.mimeType.type = RTC::RtpCodecMimeType::Type::VIDEO;
	params.mimeType.subtype = RTC::RtpCodecMimeType::Subtype::VP8;
	params.mimeType.UpdateMimeType();

	RtpStreamRecvListener listener;
	RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

	REQUIRE(rtpStream.ReceivePacket(packet) == true);
	REQUIRE(rtpStream.GetJitterUpdatedAtMs() == 0u);
	packet->SetSequenceNumber(packet->GetSequenceNumber() + 1u);
	REQUIRE(rtpStream.ReceivePacket(packet) == true);
	REQUIRE(rtpStream.GetJitterUpdatedAtMs() > 0u);
	auto* report = rtpStream.GetRtcpReceiverReport();
	REQUIRE(report);
	delete report;
	REQUIRE(rtpStream.GetRtcpLossWindowStartMs() > 0u);
	REQUIRE(rtpStream.GetRtcpLossWindowEndMs() >= rtpStream.GetRtcpLossWindowStartMs());
	REQUIRE(rtpStream.GetRtcpExpectedPackets() == 2u);
	REQUIRE(rtpStream.GetRtcpReceivedPackets() == 2u);
	REQUIRE(rtpStream.GetRtcpLostPackets() == 0u);

	flatbuffers::FlatBufferBuilder builder;
	auto statsOffset = rtpStream.FillBufferStats(builder);
	builder.Finish(statsOffset);

	const auto* stats = flatbuffers::GetRoot<FBS::RtpStream::Stats>(builder.GetBufferPointer());
	REQUIRE(stats);
	REQUIRE(stats->data_type() == FBS::RtpStream::StatsData::RecvStats);

	const auto* recvStats = stats->data_as_RecvStats();
	REQUIRE(recvStats);
	REQUIRE(recvStats->base());
	REQUIRE(recvStats->base()->data_type() == FBS::RtpStream::StatsData::BaseStats);

	const auto* baseStats = recvStats->base()->data_as_BaseStats();
	REQUIRE(baseStats);
	REQUIRE(baseStats->absCaptureTimestampNtp().has_value() == true);
	REQUIRE(baseStats->absCaptureTimestampNtp().value() == 0x0102030405060708ULL);
	REQUIRE(baseStats->estimatedCaptureClockOffset().has_value() == true);
	REQUIRE(static_cast<uint64_t>(baseStats->estimatedCaptureClockOffset().value()) == 0xfffefdfcfbfaf9f8ULL);
	REQUIRE(baseStats->rttUpdatedAtMs() == 0u);
	REQUIRE(baseStats->scoreUpdatedAtMs() > 0u);
	REQUIRE(baseStats->instantScore() == 10u);
	REQUIRE(baseStats->rtcpLossWindowStartMs() == rtpStream.GetRtcpLossWindowStartMs());
	REQUIRE(baseStats->rtcpLossWindowEndMs() == rtpStream.GetRtcpLossWindowEndMs());
	REQUIRE(baseStats->rtcpExpectedPackets() == 2u);
	REQUIRE(baseStats->rtcpReceivedPackets() == 2u);
	REQUIRE(baseStats->rtcpLostPackets() == 0u);
	REQUIRE(baseStats->newlyMissingPackets() == 0u);
	REQUIRE(baseStats->repairedPackets() == 0u);
	REQUIRE(baseStats->retransmittedPackets() == 0u);
	REQUIRE(baseStats->unrecoveredPackets() == 0u);
	REQUIRE(recvStats->jitterUpdatedAtMs() == rtpStream.GetJitterUpdatedAtMs());
	REQUIRE(recvStats->reportedBitrateWindowMs() == 2500u);
	REQUIRE(recvStats->rtpActive() == true);
	REQUIRE(recvStats->lastRtpActivityAtMs() > 0u);
	REQUIRE(recvStats->rtpActivityThresholdMs() == 0u);
	REQUIRE(recvStats->rtpActivityStateVersion() == 1u);

	const auto localWallClockMs = Utils::Time::GetRealTimeMs();
	const auto senderNtp64 = Utils::Time::UnixMsToNtp64(localWallClockMs + 1000u);
	RTCP::SenderReport senderReport;
	senderReport.SetSsrc(params.ssrc);
	senderReport.SetNtpSec(static_cast<uint32_t>(senderNtp64 >> 32));
	senderReport.SetNtpFrac(static_cast<uint32_t>(senderNtp64 & 0xFFFFFFFFULL));
	senderReport.SetRtpTs(1234u);
	rtpStream.ReceiveRtcpSenderReport(&senderReport);

	const auto senderToLocalOffsetMs = rtpStream.GetSenderToLocalClockOffsetMs();
	REQUIRE(senderToLocalOffsetMs.has_value());
	// A sender clock one second ahead of the local wall clock must produce a
	// positive sender-to-local offset, after converting both clocks to Unix ms.
	REQUIRE(senderToLocalOffsetMs.value() >= 500);
	REQUIRE(senderToLocalOffsetMs.value() <= 1500);

	delete packet;
}

SCENARIO("receive RTP stats cover normal video, no RTCP and DTX inactivity", "[rtp][rtpstream][stats]")
{
	class RtpStreamRecvListener : public RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t score, uint8_t previousScore) override
		{
			this->scores.emplace_back(previousScore, score);
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* /*rtpStream*/, RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  bool rtpActive,
		  uint64_t /*transitionAtMs*/,
		  uint64_t /*workerEventAtMs*/,
		  uint64_t lastRtpActivityAtMs,
		  uint32_t rtpActivityThresholdMs,
		  uint64_t rtpActivityStateVersion) override
		{
			this->activityTransitions.push_back(
			  { rtpActive, lastRtpActivityAtMs, rtpActivityThresholdMs, rtpActivityStateVersion });
		}

	public:
		struct ActivityTransition
		{
			bool rtpActive{ false };
			uint64_t lastRtpActivityAtMs{ 0u };
			uint32_t rtpActivityThresholdMs{ 0u };
			uint64_t rtpActivityStateVersion{ 0u };
		};

		std::vector<std::pair<uint8_t, uint8_t>> scores;
		std::vector<ActivityTransition> activityTransitions;
	};

	// clang-format off
	uint8_t buffer[] =
	{
		0x80, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x05
	};
	// clang-format on

	RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

	if (!packet)
	{
		FAIL("not a RTP packet");
	}

	RtpStream::Params params;

	params.ssrc            = packet->GetSsrc();
	params.payloadType     = packet->GetPayloadType();
	params.clockRate       = 90000;
	params.mimeType.type   = RTC::RtpCodecMimeType::Type::VIDEO;
	params.mimeType.subtype = RTC::RtpCodecMimeType::Subtype::VP8;
	params.mimeType.UpdateMimeType();

	RtpStreamRecvListener listener;
	RtpStreamRecv rtpStream(&listener, params, SendNackDelay, /*useRtpInactivityCheck*/ true);

	REQUIRE(rtpStream.ReceivePacket(packet) == true);
	packet->SetSequenceNumber(2);
	REQUIRE(rtpStream.ReceivePacket(packet) == true);

	flatbuffers::FlatBufferBuilder activeBuilder;
	auto activeStatsOffset = rtpStream.FillBufferStats(activeBuilder);
	activeBuilder.Finish(activeStatsOffset);

	const auto* activeStats = flatbuffers::GetRoot<FBS::RtpStream::Stats>(activeBuilder.GetBufferPointer());
	REQUIRE(activeStats);
	const auto* activeRecvStats = activeStats->data_as_RecvStats();
	REQUIRE(activeRecvStats);
	REQUIRE(activeRecvStats->base());
	const auto* activeBaseStats = activeRecvStats->base()->data_as_BaseStats();
	REQUIRE(activeBaseStats);

	REQUIRE(activeRecvStats->packetCount() == 2u);
	REQUIRE(activeRecvStats->byteCount() > 0u);
	REQUIRE(activeRecvStats->rtpActive() == true);
	REQUIRE(activeRecvStats->rtpActivityStateVersion() == 1u);
	REQUIRE(activeBaseStats->rttUpdatedAtMs() == 0u);
	REQUIRE(activeBaseStats->rtcpLossWindowStartMs() == 0u);
	REQUIRE(activeBaseStats->rtcpLossWindowEndMs() == 0u);
	REQUIRE(activeBaseStats->rtcpExpectedPackets() == 0u);
	REQUIRE(activeBaseStats->rtcpReceivedPackets() == 0u);
	REQUIRE(activeBaseStats->rtcpLostPackets() == 0u);

	const auto interval = rtpStream.testGetRtpInactivityCheckInterval();
	rtpStream.testSetLastRtpActivityAtMs(DepLibUV::GetTimeMs() - interval);
	rtpStream.testFireRtpInactivityTimer();

	REQUIRE(listener.scores.size() == 1u);
	const std::pair<uint8_t, uint8_t> inactiveScore{ 10u, 0u };
	REQUIRE(listener.scores.back() == inactiveScore);
	REQUIRE(listener.activityTransitions.size() == 1u);
	REQUIRE(listener.activityTransitions.back().rtpActive == false);
	REQUIRE(listener.activityTransitions.back().rtpActivityThresholdMs == interval);
	REQUIRE(listener.activityTransitions.back().rtpActivityStateVersion == 2u);

	flatbuffers::FlatBufferBuilder inactiveBuilder;
	auto inactiveStatsOffset = rtpStream.FillBufferStats(inactiveBuilder);
	inactiveBuilder.Finish(inactiveStatsOffset);

	const auto* inactiveStats = flatbuffers::GetRoot<FBS::RtpStream::Stats>(inactiveBuilder.GetBufferPointer());
	REQUIRE(inactiveStats);
	const auto* inactiveRecvStats = inactiveStats->data_as_RecvStats();
	REQUIRE(inactiveRecvStats);
	REQUIRE(inactiveRecvStats->base());
	const auto* inactiveBaseStats = inactiveRecvStats->base()->data_as_BaseStats();
	REQUIRE(inactiveBaseStats);

	REQUIRE(inactiveRecvStats->rtpActive() == false);
	REQUIRE(inactiveRecvStats->rtpActivityStateVersion() == 2u);
	REQUIRE(inactiveRecvStats->lastRtpActivityAtMs() == listener.activityTransitions.back().lastRtpActivityAtMs);
	REQUIRE(inactiveRecvStats->rtpActivityThresholdMs() == interval);
	REQUIRE(inactiveBaseStats->score() == 0u);
	REQUIRE(inactiveBaseStats->rttUpdatedAtMs() == 0u);
	REQUIRE(inactiveBaseStats->rtcpLossWindowStartMs() == 0u);
	REQUIRE(inactiveBaseStats->rtcpLossWindowEndMs() == 0u);
	REQUIRE(inactiveRecvStats->packetCount() == activeRecvStats->packetCount());
	REQUIRE(inactiveRecvStats->byteCount() == activeRecvStats->byteCount());

	delete packet;

	// Must run the loop to wait for UV timers and close them.
	DepLibUV::RunLoop();
}

SCENARIO("RTP sequence restart clears interval windows without rewriting cumulative stats", "[rtp][rtpstream][stats]")
{
	class RtpStreamRecvListener : public RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/) override
		{
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* /*rtpStream*/, RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  bool /*rtpActive*/,
		  uint64_t /*transitionAtMs*/,
		  uint64_t /*workerEventAtMs*/,
		  uint64_t /*lastRtpActivityAtMs*/,
		  uint32_t /*rtpActivityThresholdMs*/,
		  uint64_t /*rtpActivityStateVersion*/) override
		{
		}
	};

	// clang-format off
	uint8_t buffer[] =
	{
		0x80, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x05
	};
	// clang-format on

	RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

	if (!packet)
	{
		FAIL("not a RTP packet");
	}

	RtpStream::Params params;

	params.ssrc      = packet->GetSsrc();
	params.clockRate = 90000;
	params.useNack   = true;
	params.usePli    = true;

	RtpStreamRecvListener listener;
	RtpStreamRecv rtpStream(&listener, params, SendNackDelay, UseRtpInactivityCheck);

	REQUIRE(rtpStream.ReceivePacket(packet) == true);
	packet->SetSequenceNumber(2);
	REQUIRE(rtpStream.ReceivePacket(packet) == true);

	auto* report = rtpStream.GetRtcpReceiverReport();
	REQUIRE(report);
	delete report;
	REQUIRE(rtpStream.GetRtcpLossWindowStartMs() > 0u);
	REQUIRE(rtpStream.GetRtcpLossWindowEndMs() >= rtpStream.GetRtcpLossWindowStartMs());
	REQUIRE(rtpStream.GetRtcpExpectedPackets() == 2u);
	REQUIRE(rtpStream.GetRtcpReceivedPackets() == 2u);
	REQUIRE(rtpStream.GetRtcpLostPackets() == 0u);

	packet->SetSequenceNumber(40000);
	REQUIRE(rtpStream.ReceivePacket(packet) == false);

	packet->SetSequenceNumber(40001);
	REQUIRE(rtpStream.ReceivePacket(packet) == true);

	REQUIRE(rtpStream.GetNewlyMissingPackets() == 0u);
	REQUIRE(rtpStream.GetRetransmittedPackets() == 0u);
	REQUIRE(rtpStream.GetRepairedPackets() == 0u);
	REQUIRE(rtpStream.GetUnrecoveredPackets() == 0u);
	REQUIRE(rtpStream.GetRtcpLossWindowStartMs() == 0u);
	REQUIRE(rtpStream.GetRtcpLossWindowEndMs() == 0u);
	REQUIRE(rtpStream.GetRtcpExpectedPackets() == 0u);
	REQUIRE(rtpStream.GetRtcpReceivedPackets() == 0u);
	REQUIRE(rtpStream.GetRtcpLostPackets() == 0u);

	flatbuffers::FlatBufferBuilder builder;
	auto statsOffset = rtpStream.FillBufferStats(builder);
	builder.Finish(statsOffset);

	const auto* stats = flatbuffers::GetRoot<FBS::RtpStream::Stats>(builder.GetBufferPointer());
	REQUIRE(stats);
	const auto* recvStats = stats->data_as_RecvStats();
	REQUIRE(recvStats);
	REQUIRE(recvStats->base());
	const auto* baseStats = recvStats->base()->data_as_BaseStats();
	REQUIRE(baseStats);

	REQUIRE(baseStats->packetsDiscarded() == 1u);
	REQUIRE(baseStats->rtcpLossWindowStartMs() == 0u);
	REQUIRE(baseStats->rtcpLossWindowEndMs() == 0u);
	REQUIRE(baseStats->rtcpExpectedPackets() == 0u);
	REQUIRE(baseStats->rtcpReceivedPackets() == 0u);
	REQUIRE(baseStats->rtcpLostPackets() == 0u);
	REQUIRE(baseStats->newlyMissingPackets() == 0u);
	REQUIRE(baseStats->repairedPackets() == 0u);
	REQUIRE(baseStats->retransmittedPackets() == 0u);
	REQUIRE(baseStats->unrecoveredPackets() == 0u);

	delete packet;

	// Must run the loop to wait for UV timers and close them.
	DepLibUV::RunLoop();
}

SCENARIO("RTP inactivity timer keeps the last-packet deadline without per-packet restart", "[rtp][rtpstream][timer]")
{
	class RtpStreamRecvListener : public RtpStreamRecv::Listener
	{
	public:
		void OnRtpStreamScore(RtpStream* /*rtpStream*/, uint8_t score, uint8_t previousScore) override
		{
			this->scores.emplace_back(previousScore, score);
		}

		void OnRtpStreamSendRtcpPacket(RtpStreamRecv* /*rtpStream*/, RTCP::Packet* /*packet*/) override
		{
		}

		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t& /*worstRemoteFractionLost*/) override
		{
		}

		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* /*rtpStream*/,
		  bool rtpActive,
		  uint64_t transitionAtMs,
		  uint64_t workerEventAtMs,
		  uint64_t lastRtpActivityAtMs,
		  uint32_t rtpActivityThresholdMs,
		  uint64_t rtpActivityStateVersion) override
		{
			this->activityTransitions.push_back({
			  rtpActive,
			  transitionAtMs,
			  workerEventAtMs,
			  lastRtpActivityAtMs,
			  rtpActivityThresholdMs,
			  rtpActivityStateVersion
			});
		}

	public:
		struct ActivityTransition
		{
			bool rtpActive{ false };
			uint64_t transitionAtMs{ 0u };
			uint64_t workerEventAtMs{ 0u };
			uint64_t lastRtpActivityAtMs{ 0u };
			uint32_t rtpActivityThresholdMs{ 0u };
			uint64_t rtpActivityStateVersion{ 0u };
		};

		std::vector<std::pair<uint8_t, uint8_t>> scores;
		std::vector<ActivityTransition> activityTransitions;
	};

	// clang-format off
	uint8_t buffer[] =
	{
		0x80, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x05
	};
	// clang-format on

	RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

	if (!packet)
	{
		FAIL("not a RTP packet");
	}

	RtpStream::Params params;

	params.ssrc      = packet->GetSsrc();
	params.clockRate = 90000;

	{
		RtpStreamRecvListener listener;
		RtpStreamRecv rtpStream(&listener, params, SendNackDelay, /*useRtpInactivityCheck*/ true);

		const uint64_t interval = rtpStream.testGetRtpInactivityCheckInterval();

		REQUIRE(interval == 1500u);
		REQUIRE(rtpStream.GetScore() == 10u);
		REQUIRE(rtpStream.testGetRtpActivityStateVersion() == 1u);
		REQUIRE(rtpStream.testIsRtpInactivityTimerActive() == true);

		rtpStream.testSetLastRtpActivityAtMs(DepLibUV::GetTimeMs() - interval + 10u);
		rtpStream.testFireRtpInactivityTimer();

		REQUIRE(rtpStream.GetScore() == 10u);
		REQUIRE(rtpStream.testGetRtpActivityStateVersion() == 1u);
		REQUIRE(listener.scores.empty());
		REQUIRE(rtpStream.testIsRtpInactivityTimerActive() == true);

		rtpStream.testSetLastRtpActivityAtMs(DepLibUV::GetTimeMs() - interval);
		rtpStream.testFireRtpInactivityTimer();

		REQUIRE(rtpStream.GetScore() == 0u);
		REQUIRE(rtpStream.testGetRtpActivityStateVersion() == 2u);
		REQUIRE(listener.scores.size() == 1u);
		const std::pair<uint8_t, uint8_t> inactiveScore{ 10u, 0u };
		REQUIRE(listener.scores.back() == inactiveScore);
		REQUIRE(listener.activityTransitions.size() == 1u);
		REQUIRE(listener.activityTransitions.back().rtpActive == false);
		REQUIRE(listener.activityTransitions.back().rtpActivityStateVersion == 2u);
		REQUIRE(listener.activityTransitions.back().rtpActivityThresholdMs == interval);
		REQUIRE(
		  listener.activityTransitions.back().transitionAtMs ==
		  listener.activityTransitions.back().lastRtpActivityAtMs + interval);
		REQUIRE(listener.activityTransitions.back().workerEventAtMs >= listener.activityTransitions.back().transitionAtMs);
		REQUIRE(rtpStream.testIsRtpInactivityTimerActive() == false);

		packet->SetSequenceNumber(1);
		REQUIRE(rtpStream.ReceivePacket(packet) == true);

		REQUIRE(rtpStream.GetScore() == 10u);
		REQUIRE(rtpStream.testGetRtpActivityStateVersion() == 3u);
		REQUIRE(listener.scores.size() == 2u);
		const std::pair<uint8_t, uint8_t> activeScore{ 0u, 10u };
		REQUIRE(listener.scores.back() == activeScore);
		REQUIRE(listener.activityTransitions.size() == 2u);
		REQUIRE(listener.activityTransitions.back().rtpActive == true);
		REQUIRE(listener.activityTransitions.back().rtpActivityStateVersion == 3u);
		REQUIRE(listener.activityTransitions.back().rtpActivityThresholdMs == interval);
		REQUIRE(
		  listener.activityTransitions.back().transitionAtMs ==
		  listener.activityTransitions.back().lastRtpActivityAtMs);
		REQUIRE(
		  listener.activityTransitions.back().workerEventAtMs ==
		  listener.activityTransitions.back().transitionAtMs);
		REQUIRE(rtpStream.testIsRtpInactivityTimerActive() == true);

		const uint64_t firstActivityAtMs = rtpStream.testGetLastRtpActivityAtMs();

		packet->SetSequenceNumber(2);
		REQUIRE(rtpStream.ReceivePacket(packet) == true);
		REQUIRE(rtpStream.testGetLastRtpActivityAtMs() >= firstActivityAtMs);
		REQUIRE(rtpStream.GetScore() == 10u);
		REQUIRE(rtpStream.testGetRtpActivityStateVersion() == 3u);
		REQUIRE(listener.activityTransitions.size() == 2u);
	}

	delete packet;

	// Must run the loop to wait for UV timers and close them.
	DepLibUV::RunLoop();
}

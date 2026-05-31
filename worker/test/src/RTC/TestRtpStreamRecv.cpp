#include "common.hpp"
#include "DepLibUV.hpp"
#include "FBS/rtpStream.h"
#include "RTC/RtpPacket.hpp"
#include "RTC/RtpStream.hpp"
#include "RTC/RtpStreamRecv.hpp"
#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace RTC;

// 17: 16 bit mask + the initial sequence number.
static constexpr size_t MaxRequestedPackets{ 17 };
static constexpr unsigned int SendNackDelay{ 0u }; // In ms.
static const bool UseRtpInactivityCheck{ false };

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
		listener.nackedSeqNumbers.clear();

		packet->SetSequenceNumber(2);
		rtpStream.ReceivePacket(packet);

		REQUIRE(listener.nackedSeqNumbers.size() == 0);

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

	delete packet;
}

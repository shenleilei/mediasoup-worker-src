#ifndef MS_RTC_SIMPLE_CONSUMER_HPP
#define MS_RTC_SIMPLE_CONSUMER_HPP

#include "FBS/transport.h"
#include "RTC/Consumer.hpp"
#include "RTC/SeqManager.hpp"
#include "RTC/Shared.hpp"

namespace RTC
{
	class SimpleConsumer : public RTC::Consumer, public RTC::RtpStreamSend::Listener
	{
	public:
		SimpleConsumer(
		  RTC::Shared* shared,
		  const std::string& id,
		  const std::string& producerId,
		  RTC::Consumer::Listener* listener,
		  const FBS::Transport::ConsumeRequest* data);
		~SimpleConsumer() override;

	public:
		flatbuffers::Offset<FBS::Consumer::DumpResponse> FillBuffer(
		  flatbuffers::FlatBufferBuilder& builder) const;
		flatbuffers::Offset<FBS::Consumer::GetStatsResponse> FillBufferStats(
		  flatbuffers::FlatBufferBuilder& builder) override;
		flatbuffers::Offset<FBS::Consumer::ConsumerScore> FillBufferScore(
		  flatbuffers::FlatBufferBuilder& builder) const override;
		bool IsActive() const override
		{
			// clang-format off
			return (
				RTC::Consumer::IsActive() &&
				this->producerRtpStream &&
				// If there is no RTP inactivity check do not consider the stream
				// inactive despite it has score 0.
				(this->producerRtpStream->GetScore() > 0u || !this->producerRtpStream->HasRtpInactivityCheckEnabled())
			);
			// clang-format on
		}
		void ProducerRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc) override;
		void ProducerNewRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc) override;
		void ProducerRtpStreamScore(
		  RTC::RtpStreamRecv* rtpStream, uint8_t score, uint8_t previousScore) override;
		void ProducerRtcpSenderReport(RTC::RtpStreamRecv* rtpStream, bool first) override;
		uint8_t GetBitratePriority() const override;
		uint32_t IncreaseLayer(uint32_t bitrate, bool considerLoss) override;
		void ApplyLayers() override;
		uint32_t GetDesiredBitrate() const override;
		void SendRtpPacket(
		  RTC::RtpPacket* packet, RTC::Consumer::RtpPacketFanoutContext& fanoutContext) override;
		const std::vector<RTC::RtpStreamSend*>& GetRtpStreams() const override
		{
			return this->rtpStreams;
		}
		bool GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs) override;
		void NeedWorstRemoteFractionLost(uint32_t mappedSsrc, uint8_t& worstRemoteFractionLost) override;
		void ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket) override;
		void ReceiveKeyFrameRequest(RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t ssrc) override;
		void ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report) override;
		void ReceiveRtcpXrReceiverReferenceTime(RTC::RTCP::ReceiverReferenceTime* report) override;
		uint32_t GetTransmissionRate(uint64_t nowMs) override;
		float GetRtt() const override;
		uint32_t KeyFramesEmitted() const
		{
			return this->keyFramesEmitted;
		}

		/* Methods inherited from Channel::ChannelSocket::RequestHandler. */
	public:
		void HandleRequest(Channel::ChannelRequest* request) override;

	private:
		void UserOnTransportConnected() override;
		void UserOnTransportDisconnected() override;
		void UserOnPaused() override;
		void UserOnResumed() override;
		void CreateRtpStream();
		void MarkFirstFrameUnconfirmed();
		void ResolveFirstFrameConfirmation();
		void RequestKeyFrame(bool fromViewerRtcp = false, bool firstFrameRequest = false) override;
		void EmitScore() const;

		/* Pure virtual methods inherited from RtpStreamSend::Listener. */
	public:
		void OnRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore) override;
		void OnRtpStreamRetransmitRtpPacket(RTC::RtpStreamSend* rtpStream, RTC::RtpPacket* packet) override;

	private:
		// Allocated by this.
		RTC::RtpStreamSend* rtpStream{ nullptr };
		// Others.
		std::vector<RTC::RtpStreamSend*> rtpStreams;
		RTC::RtpStreamRecv* producerRtpStream{ nullptr };
		bool keyFrameSupported{ false };
		bool syncRequired{ false };
		// Viewer-confirmed first frame. A key frame handed to the transport is
		// only proof of a handoff, not proof of a viewer that can render: a new
		// consumer can win the race against the viewer's own receive path
		// (2026-09-17 ZL92061/front: handoff 0.19s after the transport came up,
		// viewer decoded nothing for a whole key-frame cadence). While such a
		// handoff stays unacknowledged the consumer keeps asking as a first-frame
		// requester, bounded by an ask budget, so the viewer is not parked on the
		// producer's key-frame cadence. Cleared when the ask budget is spent, or
		// when the viewer has both acknowledged the handed key frame through its
		// RTCP Receiver Report AND fallen quiet for KeyFrameFirstFrameUnconfirmed
		// QuietMs: an ack alone is only ring-2 evidence ("packets arrived") and
		// must not close an episode whose viewer still cannot decode (ring 3).
		bool firstFrameUnconfirmed{ false };
		// Extended (32-bit) sequence of the sync key frame handed to the
		// transport, in the same domain as the viewer's RTCP Receiver Report;
		// only meaningful while syncKeyFrameHanded is true.
		uint32_t syncKeyFrameSeq{ 0u };
		bool syncKeyFrameHanded{ false };
		// First-frame asks raised while the handoff is unconfirmed.
		uint32_t firstFrameUnconfirmedAsks{ 0u };
		uint64_t firstFrameUnconfirmedSinceMs{ 0u };
		// Time of the last VIEWER-originated first-frame ask raised while the
		// handoff is unconfirmed (0 = the viewer never asked in this episode);
		// drives the "confirmed but still asking" quiet window. Only viewer
		// asks refresh it: an internal (signaling) ask is not proof that the
		// viewer is still waiting for a decodable frame. Seeded to 0 in
		// MarkFirstFrameUnconfirmed so a viewer that acks (RR) before its first
		// PLI cannot be resolved as quiet prematurely (round-2 review R8).
		uint64_t firstFrameUnconfirmedLastAskMs{ 0u };
		// Downlink key-frame RTP packets handed by this consumer to the transport.
		// Lets the service/triage tell "SimpleConsumer really handed a key frame
		// to this viewer's transport" from "the upstream never provided one",
		// independent of the browser-side decode counters.
		uint32_t keyFramesEmitted{ 0u };
		uint64_t lastKeyFrameEvidenceAtMs{ 0u };
		RTC::SeqManager<uint16_t> rtpSeqManager;
		bool managingBitrate{ false };
		std::unique_ptr<RTC::Codecs::EncodingContext> encodingContext;
	};
} // namespace RTC

#endif

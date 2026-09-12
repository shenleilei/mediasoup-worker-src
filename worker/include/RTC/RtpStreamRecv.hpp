#ifndef MS_RTC_RTP_STREAM_RECV_HPP
#define MS_RTC_RTP_STREAM_RECV_HPP

#include "RTC/NackGenerator.hpp"
#include "RTC/RTCP/XrDelaySinceLastRr.hpp"
#include "RTC/RateCalculator.hpp"
#include "RTC/RtpStream.hpp"
#include "handles/TimerHandle.hpp"
#include <optional>
#include <vector>

namespace RTC
{
	class RtpStreamRecv : public RTC::RtpStream,
	                      public RTC::NackGenerator::Listener,
	                      public TimerHandle::Listener
	{
	public:
		class Listener : public RTC::RtpStream::Listener
		{
		public:
			virtual void OnRtpStreamSendRtcpPacket(
			  RTC::RtpStreamRecv* rtpStream, RTC::RTCP::Packet* packet) = 0;
			// Fired when the receive stream itself needs a key frame (e.g. the
			// NACK generator gave up on unrecoverable uplink loss). The listener
			// decides how to schedule the request instead of the stream sending
			// RTCP feedback on its own.
			virtual void OnRtpStreamKeyFrameRequired(RTC::RtpStreamRecv* rtpStream) = 0;
			virtual void OnRtpStreamNeedWorstRemoteFractionLost(
			  RTC::RtpStreamRecv* rtpStream, uint8_t& worstRemoteFractionLost) = 0;
			virtual void OnRtpStreamRtpActivityTransition(
			  RTC::RtpStreamRecv* rtpStream,
			  bool rtpActive,
			  uint64_t transitionAtMs,
			  uint64_t workerEventAtMs,
			  uint64_t lastRtpActivityAtMs,
			  uint32_t rtpActivityThresholdMs,
			  uint64_t rtpActivityStateVersion) = 0;
		};

	public:
		class TransmissionCounter
		{
		public:
			TransmissionCounter(uint8_t spatialLayers, uint8_t temporalLayers, size_t windowSize);
			void Update(RTC::RtpPacket* packet);
			uint32_t GetBitrate(uint64_t nowMs);
			uint32_t GetBitrate(uint64_t nowMs, uint8_t spatialLayer, uint8_t temporalLayer);
			uint32_t GetSpatialLayerBitrate(uint64_t nowMs, uint8_t spatialLayer);
			uint32_t GetLayerBitrate(uint64_t nowMs, uint8_t spatialLayer, uint8_t temporalLayer);
			size_t GetPacketCount() const;
			size_t GetBytes() const;
			size_t GetWindowSizeMs() const;

		private:
			std::vector<std::vector<RTC::RtpDataCounter>> spatialLayerCounters;
		};

	public:
		static uint8_t ComputeInstantLossScore(uint32_t expected, uint32_t lost);
		static uint8_t ComputeInstantRttScore(float rttMs);
		static uint8_t ComputeInstantScore(uint32_t expected, uint32_t lost, float rttMs);

		RtpStreamRecv(
		  RTC::RtpStreamRecv::Listener* listener,
		  RTC::RtpStream::Params& params,
		  unsigned int sendNackDelayMs,
		  bool useRtpInactivityCheck);
		~RtpStreamRecv() override;

		flatbuffers::Offset<FBS::RtpStream::Stats> FillBufferStats(
		  flatbuffers::FlatBufferBuilder& builder) override;
		bool ReceivePacket(RTC::RtpPacket* packet);
		bool ReceiveRtxPacket(RTC::RtpPacket* packet);
		RTC::RTCP::ReceiverReport* GetRtcpReceiverReport();
		RTC::RTCP::ReceiverReport* GetRtxRtcpReceiverReport();
		void ReceiveRtcpSenderReport(RTC::RTCP::SenderReport* report);
		void ReceiveRtxRtcpSenderReport(RTC::RTCP::SenderReport* report);
		void ReceiveRtcpXrDelaySinceLastRr(RTC::RTCP::DelaySinceLastRr::SsrcInfo* ssrcInfo);
		void RequestKeyFrame();
		void Pause() override;
		void Resume() override;
		uint32_t GetBitrate(uint64_t nowMs) override
		{
			return this->transmissionCounter.GetBitrate(nowMs);
		}
		uint32_t GetBitrate(uint64_t nowMs, uint8_t spatialLayer, uint8_t temporalLayer) override
		{
			return this->transmissionCounter.GetBitrate(nowMs, spatialLayer, temporalLayer);
		}
		uint32_t GetSpatialLayerBitrate(uint64_t nowMs, uint8_t spatialLayer) override
		{
			return this->transmissionCounter.GetSpatialLayerBitrate(nowMs, spatialLayer);
		}
		uint32_t GetLayerBitrate(uint64_t nowMs, uint8_t spatialLayer, uint8_t temporalLayer) override
		{
			return this->transmissionCounter.GetLayerBitrate(nowMs, spatialLayer, temporalLayer);
		}
		bool HasRtpInactivityCheckEnabled() const
		{
			return this->useRtpInactivityCheck;
		}
		uint64_t GetJitterUpdatedAtMs() const
		{
			return this->jitterUpdatedAtMs;
		}

		std::optional<int64_t> GetSenderToLocalClockOffsetMs() const
		{
			if (!this->hasSenderToLocalClockOffset) return std::nullopt;
			return this->senderToLocalClockOffsetMs;
		}

	private:
		void MarkRtpActivity();
		void CalculateJitter(uint32_t rtpTimestamp);
		void UpdateScore();
		void UpdateInstantScoreFromRtt();
		void UpdateSenderToLocalClockOffset();

	#ifdef MS_TEST
	public:
		void testReceiveRtcpXrDelaySinceLastRr(RTCP::DelaySinceLastRr::SsrcInfo* ssrcInfo)
		{
			ReceiveRtcpXrDelaySinceLastRr(ssrcInfo);
		}
		uint64_t testGetRtpInactivityCheckInterval() const
		{
			return this->rtpInactivityCheckInterval;
		}
		uint64_t testGetLastRtpActivityAtMs() const
		{
			return this->lastRtpActivityAtMs;
		}
		uint64_t testGetRtpActivityStateVersion() const
		{
			return this->rtpActivityStateVersion;
		}
		void testSetLastRtpActivityAtMs(uint64_t value)
		{
			this->lastRtpActivityAtMs = value;
		}
		bool testIsRtpInactivityTimerActive() const
		{
			return this->inactivityCheckPeriodicTimer && this->inactivityCheckPeriodicTimer->IsActive();
		}
		void testFireRtpInactivityTimer();
	#endif

		/* Pure virtual methods inherited from RTC::RtpStream. */
	public:
		void UserOnSequenceNumberGap(uint16_t seqStart, uint16_t seqEnd, uint16_t missingPackets) override;
		void UserOnSequenceNumberReset() override;

		/* Pure virtual methods inherited from TimerHandle. */
	protected:
		void OnTimer(TimerHandle* timer) override;

		/* Pure virtual methods inherited from RTC::NackGenerator. */
	protected:
		void OnNackGeneratorNackRequired(const std::vector<uint16_t>& seqNumbers) override;
		void OnNackGeneratorKeyFrameRequired() override;
		void OnNackGeneratorPacketsUnrecoverable(size_t packetCount) override;

	private:
		// Passed by argument.
		unsigned int sendNackDelayMs{ 0u };
		bool useRtpInactivityCheck{ false };
		// Others.
		// Packets expected at last interval.
		uint32_t expectedPrior{ 0u };
		// Packets expected at last interval for score calculation.
		uint32_t expectedPriorScore{ 0u };
		// Packets received at last interval.
		uint32_t receivedPrior{ 0u };
		// Packets received at last interval for score calculation.
		uint32_t receivedPriorScore{ 0u };
		// The middle 32 bits out of 64 in the NTP timestamp received in the most
		// recent sender report.
		uint32_t lastSrTimestamp{ 0u };
		// Wallclock time representing the most recent sender report arrival.
		uint64_t lastSrReceived{ 0u };
		// Wallclock counterpart used for Absolute Capture Time clock correction.
		uint64_t lastSrReceivedWallClockMs{ 0u };
		int64_t senderToLocalClockOffsetMs{ 0 };
		bool hasSenderToLocalClockOffset{ false };
		// Relative transit time for prev packet.
		int32_t transit{ 0u };
		// Jitter in RTP timestamp units. As per spec it's kept as floating value
		// although it's exposed as integer in the stats.
		float jitter{ 0 };
		uint64_t jitterUpdatedAtMs{ 0u };
		uint8_t firSeqNumber{ 0u };
		uint32_t reportedPacketLost{ 0u };
		// Latest instantaneous loss score and window loss ratio, retained so an
		// XR RTT update can recompute instantScore without advancing the SR loss
		// interval. Named distinctly from the base-class instantLossRatio metric
		// to avoid member shadowing.
		uint8_t instantLossScore{ 10u };
		float windowLossRatio{ 0.0f };
		std::unique_ptr<RTC::NackGenerator> nackGenerator;
		TimerHandle* inactivityCheckPeriodicTimer{ nullptr };
		uint64_t rtpInactivityCheckInterval{ 0u };
		uint64_t lastRtpActivityAtMs{ 0u };
		uint64_t rtpActivityStateVersion{ 1u };
		bool inactive{ false };
		// Valid media + valid RTX.
		TransmissionCounter transmissionCounter;
		// Just valid media.
		RTC::RtpDataCounter mediaTransmissionCounter;
	};
} // namespace RTC

#endif

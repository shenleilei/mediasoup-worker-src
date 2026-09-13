#ifndef MS_RTC_PRODUCER_HPP
#define MS_RTC_PRODUCER_HPP

#include "common.hpp"
#include "Channel/ChannelRequest.hpp"
#include "Channel/ChannelSocket.hpp"
#include "RTC/KeyFrameRequestManager.hpp"
#include "RTC/RTCP/CompoundPacket.hpp"
#include "RTC/RTCP/Packet.hpp"
#include "RTC/RTCP/SenderReport.hpp"
#include "RTC/RTCP/XrDelaySinceLastRr.hpp"
#include "RTC/RtpDictionaries.hpp"
#include "RTC/RtpHeaderExtensionIds.hpp"
#include "RTC/RtpPacket.hpp"
#include "RTC/RtpStreamRecv.hpp"
#include "RTC/Shared.hpp"
#include "handles/TimerHandle.hpp"
#include <absl/container/flat_hash_set.h>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace RTC
{
	class Producer : public RTC::RtpStreamRecv::Listener,
	                 public RTC::KeyFrameRequestManager::Listener,
	                 public TimerHandle::Listener,
	                 public Channel::ChannelSocket::RequestHandler,
	                 public Channel::ChannelSocket::NotificationHandler
	{
	public:
		class Listener
		{
		public:
			virtual ~Listener() = default;

		public:
			virtual void OnProducerReceiveData(RTC::Producer* producer, size_t len)                  = 0;
			virtual void OnProducerReceiveRtpPacket(RTC::Producer* producer, RTC::RtpPacket* packet) = 0;
			virtual void OnProducerPaused(RTC::Producer* producer)                                   = 0;
			virtual void OnProducerResumed(RTC::Producer* producer)                                  = 0;
			virtual void OnProducerNewRtpStream(
			  RTC::Producer* producer, RTC::RtpStreamRecv* rtpStream, uint32_t mappedSsrc) = 0;
			virtual void OnProducerRtpStreamScore(
			  RTC::Producer* producer,
			  RTC::RtpStreamRecv* rtpStream,
			  uint8_t score,
			  uint8_t previousScore) = 0;
			virtual void OnProducerRtcpSenderReport(
			  RTC::Producer* producer, RTC::RtpStreamRecv* rtpStream, bool first)                     = 0;
			virtual void OnProducerRtpPacketReceived(RTC::Producer* producer, RTC::RtpPacket* packet) = 0;
			virtual void OnProducerSendRtcpPacket(RTC::Producer* producer, RTC::RTCP::Packet* packet) = 0;
			virtual void OnProducerNeedWorstRemoteFractionLost(
			  RTC::Producer* producer, uint32_t mappedSsrc, uint8_t& worstRemoteFractionLost) = 0;
		};

	private:
		struct RtpEncodingMapping
		{
			std::string rid;
			uint32_t ssrc{ 0 };
			uint32_t mappedSsrc{ 0 };
		};

	private:
		struct RtpMapping
		{
			absl::flat_hash_map<uint8_t, uint8_t> codecs;
			std::vector<RtpEncodingMapping> encodings;
		};

	private:
		struct VideoOrientation
		{
			bool camera{ false };
			bool flip{ false };
			uint16_t rotation{ 0 };
		};

		// Bounded uplink key-frame receipt evidence.  A candidate starts only at
		// a codec-parsed frame start; it is complete only when a codec-parsed
		// (and, where needed, marker-corroborated) frame end is seen and every
		// sequence number in the start..end interval has been received.  Candidates
		// for different timestamps may coexist while RTX repair is still possible.
		struct KeyFrameCandidate
		{
			uint32_t timestamp{ 0u };
			uint16_t startSeq{ 0u };
			uint16_t endSeq{ 0u };
			uint64_t startedAtMs{ 0u };
			bool hasEnd{ false };
			bool markerSeen{ false };
			uint16_t markerSeq{ 0u };
			size_t repairedPackets{ 0u };
			std::unordered_set<uint16_t> receivedSeqs;
			TimerHandle* timer{ nullptr };
		};

		// Keeps a short packet/timestamp history so a frame-start packet that
		// arrives out of order can still recover earlier packets from the same
		// RTP timestamp.
		struct KeyFrameHistoryPacket
		{
			uint32_t timestamp{ 0u };
			bool frameEnd{ false };
			bool marker{ false };
			// Any packet of this sequence carried key-frame NAL traffic (the
			// FU start / single NAL of an IRAP NAL, including later slices of
			// a picture whose first slice was lost).
			bool keyFrameTraffic{ false };
		};

		// Snapshot of the summarized evidence state, used to detect changes
		// since the last emitted summary (the evidence flush timer only logs
		// when state actually changed).
		struct KeyFrameSummarySnapshot
		{
			uint64_t complete{ 0u };
			uint64_t incomplete{ 0u };
			uint64_t lastCompleteAtMs{ 0u };
			uint64_t lastIncompleteAtMs{ 0u };

			bool operator==(const KeyFrameSummarySnapshot& other) const
			{
				return this->complete == other.complete && this->incomplete == other.incomplete &&
				       this->lastCompleteAtMs == other.lastCompleteAtMs &&
				       this->lastIncompleteAtMs == other.lastIncompleteAtMs;
			}
		};

		struct KeyFramePacketHistory
		{
			absl::flat_hash_map<uint16_t, KeyFrameHistoryPacket> packets;
			uint16_t newestSeq{ 0u };
			bool started{ false };
		};

		enum class KeyFrameTrackResult
		{
			IGNORED = 0,
			ACTIVE,
			FINALIZED
		};

	public:
		enum class ReceiveRtpPacketResult
		{
			DISCARDED = 0,
			MEDIA     = 1,
			RETRANSMISSION
		};

	private:
		struct TraceEventTypes
		{
			bool rtp{ false };
			bool keyframe{ false };
			bool nack{ false };
			bool pli{ false };
			bool fir{ false };
			bool sr{ false };
		};

	public:
		Producer(
		  RTC::Shared* shared,
		  const std::string& id,
		  RTC::Producer::Listener* listener,
		  const FBS::Transport::ProduceRequest* data);
		~Producer() override;
		void setContext(std::string roomId, std::string peerId)
		{
			this->roomId = std::move(roomId);
			this->peerId = std::move(peerId);
		}
		const std::string& GetRoomId() const
		{
			return this->roomId;
		}
		const std::string& GetPeerId() const
		{
			return this->peerId;
		}
		std::string logPrefix() const
		{
			if (this->roomId.empty() && this->peerId.empty())
			{
				return "[" + this->id + "]";
			}

			if (this->peerId.empty())
			{
				return "[" + this->roomId + " " + this->id + "]";
			}

			return "[" + this->roomId + " " + this->peerId + " " + this->id + "]";
		}

	public:
		flatbuffers::Offset<FBS::Producer::DumpResponse> FillBuffer(
		  flatbuffers::FlatBufferBuilder& builder) const;
		flatbuffers::Offset<FBS::Producer::GetStatsResponse> FillBufferStats(
		  flatbuffers::FlatBufferBuilder& builder);
		RTC::Media::Kind GetKind() const
		{
			return this->kind;
		}
		const RTC::RtpParameters& GetRtpParameters() const
		{
			return this->rtpParameters;
		}
		const struct RTC::RtpHeaderExtensionIds& GetRtpHeaderExtensionIds() const
		{
			return this->rtpHeaderExtensionIds;
		}
		RTC::RtpParameters::Type GetType() const
		{
			return this->type;
		}
		bool IsPaused() const
		{
			return this->paused;
		}
		const absl::flat_hash_map<RTC::RtpStreamRecv*, uint32_t>& GetRtpStreams()
		{
			return this->mapRtpStreamMappedSsrc;
		}
		const std::vector<uint8_t>* GetRtpStreamScores() const
		{
			return std::addressof(this->rtpStreamScores);
		}
		ReceiveRtpPacketResult ReceiveRtpPacket(RTC::RtpPacket* packet);
		void ReceiveRtcpSenderReport(RTC::RTCP::SenderReport* report);
		void ReceiveRtcpXrDelaySinceLastRr(RTC::RTCP::DelaySinceLastRr::SsrcInfo* ssrcInfo);
		bool GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs);
		void RequestKeyFrame(uint32_t mappedSsrc, bool fromViewerRtcp);

#ifdef MS_TEST
		bool testHasKeyFrameCandidate(uint32_t ssrc) const
		{
			auto it = this->mapSsrcKeyFrameCandidates.find(ssrc);
			return it != this->mapSsrcKeyFrameCandidates.end() && !it->second.empty();
		}
		size_t testKeyFrameCandidateCount(uint32_t ssrc) const
		{
			auto it = this->mapSsrcKeyFrameCandidates.find(ssrc);
			return it == this->mapSsrcKeyFrameCandidates.end() ? 0u : it->second.size();
		}
		KeyFrameCandidate* testFindLatestKeyFrameCandidate(uint32_t ssrc) const
		{
			auto latestIt = this->mapSsrcLatestKeyFrameStartedTimestamp.find(ssrc);
			if (latestIt == this->mapSsrcLatestKeyFrameStartedTimestamp.end())
			{
				return nullptr;
			}
			auto it = this->mapSsrcKeyFrameCandidates.find(ssrc);
			if (it == this->mapSsrcKeyFrameCandidates.end())
			{
				return nullptr;
			}
			auto candidateIt = it->second.find(latestIt->second);
			return candidateIt == it->second.end() ? nullptr : candidateIt->second;
		}
		bool testKeyFrameStartHeuristicDisabled(uint32_t ssrc) const
		{
			return this->keyFrameStartDisabledSsrcs.contains(ssrc);
		}
		bool testKeyFrameIntegrityObserve() const
		{
			return this->keyFrameIntegrityObserve;
		}
		bool testSawKeyFrameTraffic(uint32_t ssrc, uint32_t timestamp) const
		{
			return SawKeyFrameTrafficForTimestamp(ssrc, timestamp);
		}
		bool testKeyFrameNoStartWarned(uint32_t ssrc) const
		{
			return this->mapSsrcLastKeyFrameNoStartWarnAtMs.contains(ssrc);
		}
		uint64_t testLastKeyFrameCompleteAtMs(uint32_t ssrc) const
		{
			auto it = this->mapSsrcLastKeyFrameCompleteAtMs.find(ssrc);
			return it == this->mapSsrcLastKeyFrameCompleteAtMs.end() ? 0u : it->second;
		}
		uint64_t testLastKeyFrameIncompleteAtMs(uint32_t ssrc) const
		{
			auto it = this->mapSsrcLastKeyFrameIncompleteAtMs.find(ssrc);
			return it == this->mapSsrcLastKeyFrameIncompleteAtMs.end() ? 0u : it->second;
		}
		uint64_t testKeyFrameSummaryEmissionCount() const
		{
			return this->keyFrameSummaryEmissions;
		}
		bool testKeyFrameEvidenceTimerActive() const
		{
			return this->keyFrameEvidenceTimer != nullptr;
		}
		void testSetKeyFrameEvidenceFlushIntervalMs(uint64_t intervalMs)
		{
			this->keyFrameEvidenceFlushIntervalMs = intervalMs;
		}
		uint64_t testCompleteKeyFrameCount(uint32_t ssrc) const
		{
			auto it = this->mapSsrcCompleteKeyFrames.find(ssrc);
			return it == this->mapSsrcCompleteKeyFrames.end() ? 0u : it->second;
		}
		uint64_t testIncompleteKeyFrameCount(uint32_t ssrc) const
		{
			auto it = this->mapSsrcIncompleteKeyFrames.find(ssrc);
			return it == this->mapSsrcIncompleteKeyFrames.end() ? 0u : it->second;
		}
		size_t testLatestKeyFrameCandidateReceivedPackets(uint32_t ssrc) const
		{
			auto* candidate = this->testFindLatestKeyFrameCandidate(ssrc);
			return candidate ? candidate->receivedSeqs.size() : 0u;
		}
		bool testLatestKeyFrameCandidateHasEnd(uint32_t ssrc) const
		{
			auto* candidate = this->testFindLatestKeyFrameCandidate(ssrc);
			return candidate && candidate->hasEnd;
		}
		uint16_t testLatestKeyFrameCandidateEndSeq(uint32_t ssrc) const
		{
			auto* candidate = this->testFindLatestKeyFrameCandidate(ssrc);
			return candidate ? candidate->endSeq : 0u;
		}
#endif

		/* Methods inherited from Channel::ChannelSocket::RequestHandler. */
	public:
		void HandleRequest(Channel::ChannelRequest* request) override;

		/* Methods inherited from Channel::ChannelSocket::NotificationHandler. */
	public:
		void HandleNotification(Channel::ChannelNotification* notification) override;

	private:
		RTC::RtpStreamRecv* GetRtpStream(RTC::RtpPacket* packet);
		RTC::RtpStreamRecv* CreateRtpStream(
		  RTC::RtpPacket* packet, const RTC::RtpCodecParameters& mediaCodec, size_t encodingIdx);
		void NotifyNewRtpStream(RTC::RtpStreamRecv* rtpStream);
		void PreProcessRtpPacket(RTC::RtpPacket* packet);
		bool MangleRtpPacket(RTC::RtpPacket* packet, RTC::RtpStreamRecv* rtpStream) const;
		void PostProcessRtpPacket(RTC::RtpPacket* packet);
		void EmitScore() const;
		void EmitRtpActivityTransition(
		  RTC::RtpStreamRecv* rtpStream,
		  bool rtpActive,
		  uint64_t transitionAtMs,
		  uint64_t workerEventAtMs,
		  uint64_t lastRtpActivityAtMs,
		  uint32_t rtpActivityThresholdMs,
		  uint64_t rtpActivityStateVersion) const;
		void EmitTraceEventRtpAndKeyFrameTypes(RTC::RtpPacket* packet, bool isRtx = false) const;
		void EmitTraceEventKeyFrameType(RTC::RtpPacket* packet, bool isRtx = false) const;
		void EmitTraceEventPliType(uint32_t ssrc) const;
		void EmitTraceEventFirType(uint32_t ssrc) const;
		void EmitTraceEventNackType() const;
		void EmitTraceEventSrType(RTC::RTCP::SenderReport* report) const;
		// Key frame cadence (see CheckKeyFrameCadence): per-SSRC baseline of the
		// last key frame seen (or the last cadence-triggered request).
		void MarkKeyFrameCadenceBaseline(uint32_t ssrc, uint64_t nowMs);
		// Records when a key frame request was last handed to the publisher
		// stream. The watchdog uses this to enforce a minimum spacing between
		// ANY two forwarded requests, not just its own fires.
		void MarkKeyFrameRequestSent(uint32_t ssrc, uint64_t nowMs);
		void CheckKeyFrameCadence(uint32_t ssrc, uint64_t nowMs);
		KeyFrameTrackResult TrackUpstreamKeyFramePacket(
		  RTC::RtpPacket* packet, bool isRtx, uint64_t nowMs);
		void StartKeyFrameCandidate(RTC::RtpPacket* packet, uint64_t nowMs);
		void FinalizeKeyFrameCandidate(
		  uint32_t ssrc,
		  KeyFrameCandidate* candidate,
		  const char* reason,
		  bool complete,
		  bool requestRecovery,
		  uint64_t nowMs);
		bool KeyFrameCandidateIsComplete(const KeyFrameCandidate& candidate) const;
		size_t CountMissingKeyFramePackets(const KeyFrameCandidate& candidate) const;
		void RecordKeyFramePacketHistory(RTC::RtpPacket* packet);
		void ClearKeyFrameCandidate(uint32_t ssrc, const char* reason, bool requestRecovery);
		void ClearKeyFrameCandidates(const char* reason, bool requestRecovery);
		void MaybeLogKeyFrameSummary(uint32_t ssrc, uint64_t nowMs, bool force = false);
		void WarnKeyFrameEndWithoutStart(uint32_t ssrc, uint32_t timestamp, uint16_t seq, uint64_t nowMs);
		bool SawKeyFrameTrafficForTimestamp(uint32_t ssrc, uint32_t timestamp) const;
		void StartKeyFrameEvidenceTimerIfNeeded();
		void StopKeyFrameEvidenceTimer() noexcept;
		void FlushDirtyKeyFrameSummaries(uint64_t nowMs);
		bool KeyFrameEvidenceDirty(uint32_t ssrc) const;
		KeyFrameSummarySnapshot CurrentKeyFrameSummarySnapshot(uint32_t ssrc) const;
		void CollectKeyFrameEvidenceSsrcs(absl::flat_hash_set<uint32_t>& ssrcs) const;
		void EmitTraceEvent(flatbuffers::Offset<FBS::Producer::TraceNotification>& notification) const;

		/* Pure virtual methods inherited from RTC::RtpStreamRecv::Listener. */
	public:
		void OnRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore) override;
		void OnRtpStreamSendRtcpPacket(RTC::RtpStreamRecv* rtpStream, RTC::RTCP::Packet* packet) override;
		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* rtpStream, uint8_t& worstRemoteFractionLost) override;
		void OnRtpStreamRtpActivityTransition(
		  RTC::RtpStreamRecv* rtpStream,
		  bool rtpActive,
		  uint64_t transitionAtMs,
		  uint64_t workerEventAtMs,
		  uint64_t lastRtpActivityAtMs,
		  uint32_t rtpActivityThresholdMs,
		  uint64_t rtpActivityStateVersion) override;
		void OnRtpStreamKeyFrameRequired(RTC::RtpStreamRecv* rtpStream) override;

		/* Pure virtual methods inherited from TimerHandle::Listener. */
	public:
		void OnTimer(TimerHandle* timer) override;

		/* Pure virtual methods inherited from RTC::KeyFrameRequestManager::Listener. */
	public:
		void OnKeyFrameNeeded(RTC::KeyFrameRequestManager* keyFrameRequestManager, uint32_t ssrc) override;

	public:
		// Passed by argument.
		const std::string id;

	private:
		// Passed by argument.
		RTC::Shared* shared{ nullptr };
		RTC::Producer::Listener* listener{ nullptr };
		// Allocated by this.
		absl::flat_hash_map<uint32_t, RTC::RtpStreamRecv*> mapSsrcRtpStream;
		RTC::KeyFrameRequestManager* keyFrameRequestManager{ nullptr };
		// 0 means legacy behavior: no viewer-request suppression, no cadence
		// watchdog, and no request coalescing window.
		uint32_t keyFrameRequestDelay{ 0u };
		// Observation-only key frame integrity tracking, enabled by the
		// MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE=observe worker env var.
		// When true, candidates/history run and complete/incomplete evidence is
		// logged, but results never clear pending requests, refresh the cadence
		// baseline or trigger recovery requests.
		bool keyFrameIntegrityObserve{ false };
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcKeyFrameCadenceAtMs;
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcLastKeyFrameRequestAtMs;
		absl::flat_hash_map<uint32_t, std::map<uint32_t, KeyFrameCandidate*>> mapSsrcKeyFrameCandidates;
		absl::flat_hash_map<uint32_t, uint32_t> mapSsrcLatestKeyFrameStartedTimestamp;
		absl::flat_hash_map<uint32_t, KeyFramePacketHistory> mapSsrcKeyFramePacketHistory;
		// SSRCs whose codec-level first-slice evidence proved contradictory
		// (two distinct first-slice markers within one picture, e.g. H.264
		// FMO).  Frame-start candidates are no longer started for these SSRCs.
		absl::flat_hash_set<uint32_t> keyFrameStartDisabledSsrcs;
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcCompleteKeyFrames;
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcIncompleteKeyFrames;
		// Rate-limited per-SSRC evidence summary timestamp (worker INFO level).
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcLastKeyFrameSummaryAtMs;
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcLastKeyFrameNoStartWarnAtMs;
		// Last complete/incomplete evidence times, included in summaries and
		// forced dumps so a frozen stream can answer "when did the SFU last
		// receive a complete key frame".
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcLastKeyFrameCompleteAtMs;
		absl::flat_hash_map<uint32_t, uint64_t> mapSsrcLastKeyFrameIncompleteAtMs;
		// Evidence flush timer: a repeating timer that flushes the latest
		// key frame evidence when a stream stops sending while the producer
		// stays alive (single-stream video has no RTP-inactivity score event).
		// It only runs while unsent evidence exists and never influences
		// scoring or request scheduling.
		TimerHandle* keyFrameEvidenceTimer{ nullptr };
		// Set during destruction: candidate cleanup may finalize incomplete
		// frames and would otherwise restart the evidence timer for an object
		// that is about to be freed (use-after-free on the timer callback).
		bool keyFrameEvidenceTimerClosed{ false };
		uint64_t keyFrameEvidenceFlushIntervalMs{ 60000u };
		absl::flat_hash_map<uint32_t, KeyFrameSummarySnapshot> mapSsrcKeyFrameSummarySnapshot;
		uint64_t keyFrameSummaryEmissions{ 0u };
		// Viewer-request suppression diagnostics: cumulative count plus a
		// rate-limited WARN so freeze incidents can prove "viewers asked, the
		// cadence policy held them back" without flooding the log.
		uint64_t suppressedViewerKeyFrameRequests{ 0u };
		uint64_t lastSuppressedViewerRequestLogAtMs{ 0u };
		// Others.
		RTC::Media::Kind kind;
		RTC::RtpParameters rtpParameters;
		RTC::RtpParameters::Type type;
		struct RtpMapping rtpMapping;
		std::vector<RTC::RtpStreamRecv*> rtpStreamByEncodingIdx;
		std::vector<uint8_t> rtpStreamScores;
		absl::flat_hash_map<uint32_t, RTC::RtpStreamRecv*> mapRtxSsrcRtpStream;
		absl::flat_hash_map<RTC::RtpStreamRecv*, uint32_t> mapRtpStreamMappedSsrc;
		absl::flat_hash_map<uint32_t, uint32_t> mapMappedSsrcSsrc;
		struct RTC::RtpHeaderExtensionIds rtpHeaderExtensionIds;
		bool paused{ false };
		RTC::RtpPacket* currentRtpPacket{ nullptr };
		// Timestamp when last RTCP was sent.
		uint64_t lastRtcpSentTime{ 0u };
		uint16_t maxRtcpInterval{ 0u };
		// Video orientation.
		bool videoOrientationDetected{ false };
		struct VideoOrientation videoOrientation;
		struct TraceEventTypes traceEventTypes;
		std::string roomId;
		std::string peerId;
		// Static buffer.
		thread_local static uint8_t* buffer;
	};
} // namespace RTC

#endif

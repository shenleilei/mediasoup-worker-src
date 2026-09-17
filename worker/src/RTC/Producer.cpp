#define MS_CLASS "RTC::Producer"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/Producer.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include "RTC/Codecs/Tools.hpp"
#include "RTC/RTCP/Feedback.hpp"
#include "RTC/RTCP/XrReceiverReferenceTime.hpp"
#include <absl/container/inlined_vector.h>
#include <algorithm> // std::min()
#include <cstdlib>   // std::getenv()
#include <cstring>   // std::memcpy()
#include <string_view>

namespace RTC
{
	/* Static variables. */

	thread_local uint8_t* Producer::buffer{ nullptr };

	/* Static. */

	static constexpr unsigned int SendNackDelay{ 10u }; // In ms.
	static constexpr uint64_t KeyFrameCandidateTimeoutMs{ 500u };
	// Minimum spacing between first-frame bypass sends so simultaneous
	// consumer creations cannot burst the publisher.
	static constexpr uint64_t KeyFrameFirstFrameRequestSpacingMs{ 1000u };
	// Viewer asks for an unacknowledged first-frame handoff are the urgent
	// case (that viewer already lost one key frame), so they are spaced
	// tighter than the internal first-frame path. The per-SSRC spacing is
	// what keeps a mass join from becoming a publisher request burst.
	static constexpr uint64_t KeyFrameFirstFrameViewerSpacingMs{ 500u };
	// New-viewer early pass: a first-frame request is served immediately when
	// New-viewer early pass fold window: a first-frame request is folded into
	// the next scheduled key frame release when it is imminent (within this
	// window from the release), and is sent immediately otherwise (weekly
	// review 2026-09-15, user feature).  OSS recording also requests key frames
	// on this producer, so the 1s force-spacing is frequently "recently
	// refreshed" by the recording and a real viewer's first frame must not
	// wait out the watchdog cadence.  At runtime the window is additionally
	// capped at keyFrameRequestDelay/2 so a small delay cannot make the force
	// branch dead code.
	static constexpr uint64_t KeyFrameFirstFrameFoldWindowMs{ 2000u };
	// Production-visible evidence cadence (worker INFO): one summary per SSRC
	// per interval; bounded missing-sequence list on incomplete WARNs.
	static constexpr uint64_t KeyFrameSummaryIntervalMs{ 60000u };
	static constexpr size_t KeyFrameMissingSeqListMax{ 16u };

	// Observation-only key frame integrity mode.  Parsed per producer
	// construction (one getenv per producer, negligible) so tests can toggle
	// the mode inside one process.
	bool ConfiguredKeyFrameIntegrityObserve()
	{
		const char* raw = std::getenv("MEDIASOUP_VIDEO_KEY_FRAME_INTEGRITY_MODE");

		return raw != nullptr && std::string_view(raw) == "observe";
	}
	static constexpr size_t KeyFrameHistoryMaxPackets{ 4096u };
	static constexpr uint16_t KeyFrameMaxSequenceSpan{ 4096u };

	/* Instance methods. */

	Producer::Producer(
	  RTC::Shared* shared,
	  const std::string& id,
	  RTC::Producer::Listener* listener,
	  const FBS::Transport::ProduceRequest* data)
	  : id(id), shared(shared), listener(listener), kind(RTC::Media::Kind(data->kind()))
	{
		MS_TRACE();

		if (this->kind == RTC::Media::Kind::VIDEO && ConfiguredKeyFrameIntegrityObserve())
		{
			this->keyFrameIntegrityObserve = true;

			MS_EVIDENCE_INFO(
			  "video key frame integrity observation enabled [producerId:%s, keyFrameRequestDelay:%" PRIu32
			  ", mode:observe]",
			  id.c_str(),
			  data->keyFrameRequestDelay());
		}

		// This may throw.
		this->rtpParameters = RTC::RtpParameters(data->rtpParameters());

		// Evaluate type.
		auto type = RTC::RtpParameters::GetType(this->rtpParameters);

		if (!type.has_value())
		{
			MS_THROW_TYPE_ERROR("invalid RTP parameters");
		}

		this->type = type.value();

		// Reserve a slot in rtpStreamByEncodingIdx and rtpStreamsScores vectors
		// for each RTP stream.
		this->rtpStreamByEncodingIdx.resize(this->rtpParameters.encodings.size(), nullptr);
		this->rtpStreamScores.resize(this->rtpParameters.encodings.size(), 0u);

		auto& encoding         = this->rtpParameters.encodings[0];
		const auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		if (!RTC::Codecs::Tools::IsValidTypeForCodec(this->type, mediaCodec->mimeType))
		{
			MS_THROW_TYPE_ERROR(
			  "%s codec not supported for %s",
			  mediaCodec->mimeType.ToString().c_str(),
			  RTC::RtpParameters::GetTypeString(this->type).c_str());
		}

		for (const auto& codec : *data->rtpMapping()->codecs())
		{
			this->rtpMapping.codecs[codec->payloadType()] = codec->mappedPayloadType();
		}

		const auto* encodings = data->rtpMapping()->encodings();

		this->rtpMapping.encodings.reserve(encodings->size());

		for (const auto& encoding : *encodings)
		{
			this->rtpMapping.encodings.emplace_back();

			auto& encodingMapping = this->rtpMapping.encodings.back();

			// ssrc is optional.
			if (encoding->ssrc().has_value())
			{
				encodingMapping.ssrc = encoding->ssrc().value();
			}

			// rid is optional.
			// However ssrc or rid must be present (if more than 1 encoding).
			// clang-format off
			if (
				encodings->size() > 1 &&
				!encoding->ssrc().has_value() &&
				!flatbuffers::IsFieldPresent(encoding, FBS::RtpParameters::EncodingMapping::VT_RID)
			)
			// clang-format on
			{
				MS_THROW_TYPE_ERROR("wrong entry in rtpMapping.encodings (missing ssrc or rid)");
			}

			// If there is no mid and a single encoding, ssrc or rid must be present.
			// clang-format off
			if (
				this->rtpParameters.mid.empty() &&
				encodings->size() == 1 &&
				!encoding->ssrc().has_value() &&
				!flatbuffers::IsFieldPresent(encoding, FBS::RtpParameters::EncodingMapping::VT_RID)
			)
			// clang-format on
			{
				MS_THROW_TYPE_ERROR(
				  "wrong entry in rtpMapping.encodings (missing ssrc or rid, or rtpParameters.mid)");
			}

			// mappedSsrc is mandatory.
			if (!encoding->mappedSsrc())
			{
				MS_THROW_TYPE_ERROR("wrong entry in rtpMapping.encodings (missing mappedSsrc)");
			}

			encodingMapping.mappedSsrc = encoding->mappedSsrc();
		}

		this->paused = data->paused();

		// The number of encodings in rtpParameters must match the number of encodings
		// in rtpMapping.
		if (this->rtpParameters.encodings.size() != this->rtpMapping.encodings.size())
		{
			MS_THROW_TYPE_ERROR("rtpParameters.encodings size does not match rtpMapping.encodings size");
		}

		// Fill RTP header extension ids.
		// This may throw.
		for (auto& exten : this->rtpParameters.headerExtensions)
		{
			if (exten.id == 0u)
			{
				MS_THROW_TYPE_ERROR("RTP extension id cannot be 0");
			}

			if (this->rtpHeaderExtensionIds.mid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::MID)
			{
				this->rtpHeaderExtensionIds.mid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.rid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::RTP_STREAM_ID)
			{
				this->rtpHeaderExtensionIds.rid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.rrid == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::REPAIRED_RTP_STREAM_ID)
			{
				this->rtpHeaderExtensionIds.rrid = exten.id;
			}

			if (this->rtpHeaderExtensionIds.absSendTime == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME)
			{
				this->rtpHeaderExtensionIds.absSendTime = exten.id;
			}

			if (this->rtpHeaderExtensionIds.transportWideCc01 == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01)
			{
				this->rtpHeaderExtensionIds.transportWideCc01 = exten.id;
			}

			// NOTE: Remove this once framemarking draft becomes RFC.
			if (this->rtpHeaderExtensionIds.frameMarking07 == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::FRAME_MARKING_07)
			{
				this->rtpHeaderExtensionIds.frameMarking07 = exten.id;
			}

			if (this->rtpHeaderExtensionIds.frameMarking == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::FRAME_MARKING)
			{
				this->rtpHeaderExtensionIds.frameMarking = exten.id;
			}

			if (this->rtpHeaderExtensionIds.ssrcAudioLevel == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::SSRC_AUDIO_LEVEL)
			{
				this->rtpHeaderExtensionIds.ssrcAudioLevel = exten.id;
			}

			if (this->rtpHeaderExtensionIds.videoOrientation == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::VIDEO_ORIENTATION)
			{
				this->rtpHeaderExtensionIds.videoOrientation = exten.id;
			}

			if (this->rtpHeaderExtensionIds.toffset == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::TOFFSET)
			{
				this->rtpHeaderExtensionIds.toffset = exten.id;
			}

			if (this->rtpHeaderExtensionIds.absCaptureTime == 0u && exten.type == RTC::RtpHeaderExtensionUri::Type::ABS_CAPTURE_TIME)
			{
				this->rtpHeaderExtensionIds.absCaptureTime = exten.id;
			}
		}

		// Set the RTCP report generation interval.
		if (this->kind == RTC::Media::Kind::AUDIO)
		{
			this->maxRtcpInterval = RTC::RTCP::MaxAudioIntervalMs;
		}
		else
		{
			this->maxRtcpInterval = RTC::RTCP::MaxVideoIntervalMs;
		}

		// Create a KeyFrameRequestManager.
		if (this->kind == RTC::Media::Kind::VIDEO)
		{
			this->keyFrameRequestDelay = data->keyFrameRequestDelay();

			this->keyFrameRequestManager =
			  new RTC::KeyFrameRequestManager(this, this->keyFrameRequestDelay);
		}

		// NOTE: This may throw.
		this->shared->channelMessageRegistrator->RegisterHandler(
		  this->id,
		  /*channelRequestHandler*/ this,
		  /*channelNotificationHandler*/ this);
	}

	Producer::~Producer()
	{
		MS_TRACE();

		this->shared->channelMessageRegistrator->UnregisterHandler(this->id);

		// Stop the evidence timer and forbid restarts: candidate cleanup below
		// finalizes incomplete frames, which would otherwise start a new timer
		// whose callback targets this object after destruction.
		this->keyFrameEvidenceTimerClosed = true;
		StopKeyFrameEvidenceTimer();

		// Stop candidate timers before any listener/manager teardown.  This
		// may finalize active candidates as incomplete; the final evidence
		// dump below must run after it so those results are included.
		ClearKeyFrameCandidates("producer_closed", /*requestRecovery=*/false);

		// Final evidence dump: a closed producer cannot emit later summaries,
		// so the last observed state must be flushed now.
		{
			absl::flat_hash_set<uint32_t> evidenceSsrcs;
			CollectKeyFrameEvidenceSsrcs(evidenceSsrcs);
			const uint64_t nowMs = DepLibUV::GetTimeMs();
			for (const auto ssrc : evidenceSsrcs)
			{
				MaybeLogKeyFrameSummary(ssrc, nowMs, /*force=*/true);
			}
		}

		// Delete all streams.
		for (auto& kv : this->mapSsrcRtpStream)
		{
			auto* rtpStream = kv.second;

			delete rtpStream;
		}

		this->mapSsrcRtpStream.clear();
		this->rtpStreamByEncodingIdx.clear();
		this->rtpStreamScores.clear();
		this->mapRtxSsrcRtpStream.clear();
		this->mapRtpStreamMappedSsrc.clear();
		this->mapMappedSsrcSsrc.clear();

		// Delete the KeyFrameRequestManager.
		delete this->keyFrameRequestManager;
	}

	flatbuffers::Offset<FBS::Producer::DumpResponse> Producer::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Add rtpParameters.
		auto rtpParameters = this->rtpParameters.FillBuffer(builder);

		// Add rtpMapping.codecs.
		std::vector<flatbuffers::Offset<FBS::RtpParameters::CodecMapping>> codecs;

		for (const auto& kv : this->rtpMapping.codecs)
		{
			codecs.emplace_back(FBS::RtpParameters::CreateCodecMapping(builder, kv.first, kv.second));
		}

		// Add rtpMapping.encodings.
		std::vector<flatbuffers::Offset<FBS::RtpParameters::EncodingMapping>> encodings;
		encodings.reserve(this->rtpMapping.encodings.size());

		for (const auto& encodingMapping : this->rtpMapping.encodings)
		{
			encodings.emplace_back(FBS::RtpParameters::CreateEncodingMappingDirect(
			  builder,
			  encodingMapping.rid.c_str(),
			  encodingMapping.ssrc != 0u ? flatbuffers::Optional<uint32_t>(encodingMapping.ssrc)
			                             : flatbuffers::nullopt,
			  nullptr, /* capability mode. NOTE: Present in NODE*/
			  encodingMapping.mappedSsrc));
		}

		// Build rtpMapping.
		auto rtpMapping = FBS::RtpParameters::CreateRtpMappingDirect(builder, &codecs, &encodings);

		// Add rtpStreams.
		std::vector<flatbuffers::Offset<FBS::RtpStream::Dump>> rtpStreams;

		for (const auto* rtpStream : this->rtpStreamByEncodingIdx)
		{
			if (!rtpStream)
			{
				continue;
			}

			rtpStreams.emplace_back(rtpStream->FillBuffer(builder));
		}

		// Add traceEventTypes.
		std::vector<FBS::Producer::TraceEventType> traceEventTypes;

		if (this->traceEventTypes.rtp)
		{
			traceEventTypes.emplace_back(FBS::Producer::TraceEventType::RTP);
		}
		if (this->traceEventTypes.keyframe)
		{
			traceEventTypes.emplace_back(FBS::Producer::TraceEventType::KEYFRAME);
		}
		if (this->traceEventTypes.nack)
		{
			traceEventTypes.emplace_back(FBS::Producer::TraceEventType::NACK);
		}
		if (this->traceEventTypes.pli)
		{
			traceEventTypes.emplace_back(FBS::Producer::TraceEventType::PLI);
		}
		if (this->traceEventTypes.fir)
		{
			traceEventTypes.emplace_back(FBS::Producer::TraceEventType::FIR);
		}

		return FBS::Producer::CreateDumpResponseDirect(
		  builder,
		  this->id.c_str(),
		  this->kind == RTC::Media::Kind::AUDIO ? FBS::RtpParameters::MediaKind::AUDIO
		                                        : FBS::RtpParameters::MediaKind::VIDEO,
		  RTC::RtpParameters::TypeToFbs(this->type),
		  rtpParameters,
		  rtpMapping,
		  &rtpStreams,
		  &traceEventTypes,
		  this->paused);
	}

	flatbuffers::Offset<FBS::Producer::GetStatsResponse> Producer::FillBufferStats(
	  flatbuffers::FlatBufferBuilder& builder)
	{
		MS_TRACE();

		std::vector<flatbuffers::Offset<FBS::RtpStream::Stats>> rtpStreams;

		for (auto* rtpStream : this->rtpStreamByEncodingIdx)
		{
			if (!rtpStream)
			{
				continue;
			}

			rtpStreams.emplace_back(rtpStream->FillBufferStats(builder));
		}

		return FBS::Producer::CreateGetStatsResponseDirect(builder, &rtpStreams);
	}

	void Producer::HandleRequest(Channel::ChannelRequest* request)
	{
		MS_TRACE();

		switch (request->method)
		{
			case Channel::ChannelRequest::Method::PRODUCER_DUMP:
			{
				auto dumpOffset = FillBuffer(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::Producer_DumpResponse, dumpOffset);

				break;
			}

			case Channel::ChannelRequest::Method::PRODUCER_GET_STATS:
			{
				auto responseOffset = FillBufferStats(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::Producer_GetStatsResponse, responseOffset);

				break;
			}

			case Channel::ChannelRequest::Method::PRODUCER_PAUSE:
			{
				if (this->paused)
				{
					request->Accept();

					break;
				}

				// A paused producer does not forward media.  Resume already forces a
				// fresh key frame, so retain no partial candidate across pause.
				ClearKeyFrameCandidates("paused", /*requestRecovery=*/false);

				// Pausing is a stream-stop-like boundary: flush pending evidence.
				{
					absl::flat_hash_set<uint32_t> evidenceSsrcs;
					CollectKeyFrameEvidenceSsrcs(evidenceSsrcs);
					const uint64_t nowMs = DepLibUV::GetTimeMs();
					for (const auto ssrc : evidenceSsrcs)
					{
						MaybeLogKeyFrameSummary(ssrc, nowMs, /*force=*/true);
					}
				}

				// Pause all streams.
				for (auto& kv : this->mapSsrcRtpStream)
				{
					auto* rtpStream = kv.second;

					rtpStream->Pause();
				}

				this->paused = true;

				MS_DEBUG_DEV("%s Producer paused", this->logPrefix());

				this->listener->OnProducerPaused(this);

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::PRODUCER_RESUME:
			{
				if (!this->paused)
				{
					request->Accept();

					break;
				}

				// Resume all streams.
				for (auto& kv : this->mapSsrcRtpStream)
				{
					auto* rtpStream = kv.second;

					rtpStream->Resume();
				}

				this->paused = false;

				MS_DEBUG_DEV("%s Producer resumed", this->logPrefix());

				this->listener->OnProducerResumed(this);

				if (this->keyFrameRequestManager)
				{
					MS_DEBUG_2TAGS(rtcp, rtx, "requesting forced key frame(s) after resumed");

					// Request a key frame for all streams.
					const uint64_t resumeAtMs = DepLibUV::GetTimeMs();

					for (auto& kv : this->mapSsrcRtpStream)
					{
						auto ssrc = kv.first;

						this->keyFrameRequestManager->ForceKeyFrameNeeded(ssrc);

						MarkKeyFrameCadenceBaseline(ssrc, resumeAtMs);
					}
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::PRODUCER_ENABLE_TRACE_EVENT:
			{
				const auto* body = request->data->body_as<FBS::Producer::EnableTraceEventRequest>();

				// Reset traceEventTypes.
				struct TraceEventTypes newTraceEventTypes;

				for (const auto& type : *body->events())
				{
					switch (type)
					{
						case FBS::Producer::TraceEventType::KEYFRAME:
						{
							newTraceEventTypes.keyframe = true;

							break;
						}
						case FBS::Producer::TraceEventType::FIR:
						{
							newTraceEventTypes.fir = true;

							break;
						}
						case FBS::Producer::TraceEventType::NACK:
						{
							newTraceEventTypes.nack = true;

							break;
						}
						case FBS::Producer::TraceEventType::PLI:
						{
							newTraceEventTypes.pli = true;

							break;
						}
						case FBS::Producer::TraceEventType::RTP:
						{
							newTraceEventTypes.rtp = true;

							break;
						}
						case FBS::Producer::TraceEventType::SR:
						{
							newTraceEventTypes.sr = true;

							break;
						}
					}
				}

				this->traceEventTypes = newTraceEventTypes;

				request->Accept();

				break;
			}

			default:
			{
				MS_THROW_ERROR("unknown method '%s'", request->methodCStr);
			}
		}
	}

	void Producer::HandleNotification(Channel::ChannelNotification* notification)
	{
		MS_TRACE();

		switch (notification->event)
		{
			case Channel::ChannelNotification::Event::PRODUCER_SEND:
			{
				const auto* body = notification->data->body_as<FBS::Producer::SendNotification>();
				auto len         = body->data()->size();

				// Increase receive transmission.
				this->listener->OnProducerReceiveData(this, len);

				if (len > RTC::MtuSize + 100)
				{
					MS_WARN_TAG(info, "given RTP packet exceeds maximum size [len:%i]", len);

					break;
				}

				// If this is the first time to receive a RTP packet then allocate the
				// receiving buffer now.
				if (!Producer::buffer)
				{
					Producer::buffer = new uint8_t[RTC::MtuSize + 100];
				}

				// Copy the received packet into this buffer so it can be expanded later.
				std::memcpy(Producer::buffer, body->data()->data(), static_cast<size_t>(len));

				RTC::RtpPacket* packet =
				  RTC::RtpPacket::Parse(Producer::buffer, len, RTC::MtuSize + 100u);

				if (!packet)
				{
					MS_WARN_TAG(info, "received data is not a valid RTP packet");

					break;
				}

				// Pass the packet to the parent transport.
				this->listener->OnProducerReceiveRtpPacket(this, packet);

				break;
			}

			default:
			{
				MS_ERROR("%s unknown event '%s'", this->logPrefix().c_str(), notification->eventCStr);
			}
		}
	}

	Producer::ReceiveRtpPacketResult Producer::ReceiveRtpPacket(RTC::RtpPacket* packet)
	{
		MS_TRACE();

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.producerId = this->id;
#endif

		// Reset current packet.
		this->currentRtpPacket = nullptr;

		// Count number of RTP streams.
		auto numRtpStreamsBefore = this->mapSsrcRtpStream.size();

		auto* rtpStream = GetRtpStream(packet);

		if (!rtpStream)
		{
			MS_WARN_TAG(info, "no stream found for received packet [ssrc:%" PRIu32 "]", packet->GetSsrc());

#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::RECV_RTP_STREAM_NOT_FOUND);
#endif

			return ReceiveRtpPacketResult::DISCARDED;
		}

		// Pre-process the packet.
		PreProcessRtpPacket(packet);

		ReceiveRtpPacketResult result;
		bool isRtx{ false };

		// Media packet.
		if (packet->GetSsrc() == rtpStream->GetSsrc())
		{
			result = ReceiveRtpPacketResult::MEDIA;

			// Process the packet.
			if (!rtpStream->ReceivePacket(packet))
			{
				// May have to announce a new RTP stream to the listener.
				if (this->mapSsrcRtpStream.size() > numRtpStreamsBefore)
				{
					NotifyNewRtpStream(rtpStream);
				}

#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::RECV_RTP_STREAM_DISCARDED);
#endif

				return result;
			}
		}
		// RTX packet.
		else if (packet->GetSsrc() == rtpStream->GetRtxSsrc())
		{
			result = ReceiveRtpPacketResult::RETRANSMISSION;
			isRtx  = true;

			// Process the packet.
			if (!rtpStream->ReceiveRtxPacket(packet))
			{
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::RECV_RTP_STREAM_NOT_FOUND);
#endif

				return result;
			}
		}
		// Should not happen.
		else
		{
			MS_ABORT("found stream does not match received packet");
		}

		const uint64_t nowMs = DepLibUV::GetTimeMs();

		// Cadence mode requires an actually received (complete) key frame before
		// it clears pending requests or refreshes the cadence baseline.  Legacy
		// mode (delay 0) intentionally keeps the historical first-fragment
		// behavior.
		KeyFrameTrackResult keyFrameTrackResult{ KeyFrameTrackResult::IGNORED };
		const bool cadenceTracking =
		  this->keyFrameRequestDelay > 0u && this->keyFrameRequestManager != nullptr;

		if (cadenceTracking)
		{
			RecordKeyFramePacketHistory(packet);
			keyFrameTrackResult = TrackUpstreamKeyFramePacket(packet, isRtx, nowMs);
		}
		else if (packet->IsKeyFrame())
		{
			MS_DEBUG_TAG(
			  rtp,
			  "key frame received [ssrc:%" PRIu32 ", seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber());

			// Tell the keyFrameRequestManager.
			if (this->keyFrameRequestManager)
			{
				this->keyFrameRequestManager->KeyFrameReceived(packet->GetSsrc());
			}

			// A received key frame refreshes the cadence baseline.
			MarkKeyFrameCadenceBaseline(packet->GetSsrc(), nowMs);
		}

		// Observation-only bypass for legacy mode (delay 0): track key frame
		// candidates without touching request scheduling.  The legacy
		// KeyFrameReceived/baseline handling above must keep running unchanged;
		// FinalizeKeyFrameCandidate enforces nothing in legacy mode.  In
		// cadence mode (delay > 0) tracking already ran above, so observation
		// adds nothing here.
		if (this->keyFrameIntegrityObserve && !cadenceTracking)
		{
			RecordKeyFramePacketHistory(packet);
			keyFrameTrackResult = TrackUpstreamKeyFramePacket(packet, isRtx, nowMs);
		}

		if (
			!packet->IsKeyFrame() && !isRtx &&
			keyFrameTrackResult == KeyFrameTrackResult::IGNORED)
		{
			// Lazy key frame cadence watchdog, evaluated at packet cadence.
			CheckKeyFrameCadence(packet->GetSsrc(), nowMs);
		}

		// May have to announce a new RTP stream to the listener.
		if (this->mapSsrcRtpStream.size() > numRtpStreamsBefore)
		{
			// Request a key frame for this stream since we may have lost the first packets
			// (do not do it if this is a key frame).
			if (this->keyFrameRequestManager && !this->paused && !packet->IsKeyFrame())
			{
				this->keyFrameRequestManager->ForceKeyFrameNeeded(packet->GetSsrc());

				MarkKeyFrameCadenceBaseline(packet->GetSsrc(), nowMs);
			}

			// Update current packet.
			this->currentRtpPacket = packet;

			NotifyNewRtpStream(rtpStream);

			// Reset current packet.
			this->currentRtpPacket = nullptr;
		}

		// If paused stop here.
		if (this->paused)
		{
			return result;
		}

		// May emit 'trace' event.
		EmitTraceEventRtpAndKeyFrameTypes(packet, isRtx);

		// Mangle the packet before providing the listener with it.
		if (!MangleRtpPacket(packet, rtpStream))
		{
			return ReceiveRtpPacketResult::DISCARDED;
		}

		// Post-process the packet.
		PostProcessRtpPacket(packet);

		this->listener->OnProducerRtpPacketReceived(this, packet);

		return result;
	}

	void Producer::ReceiveRtcpSenderReport(RTC::RTCP::SenderReport* report)
	{
		MS_TRACE();

		auto it = this->mapSsrcRtpStream.find(report->GetSsrc());

		if (it != this->mapSsrcRtpStream.end())
		{
			auto* rtpStream  = it->second;
			const bool first = rtpStream->GetSenderReportNtpMs() == 0;

			rtpStream->ReceiveRtcpSenderReport(report);

			this->listener->OnProducerRtcpSenderReport(this, rtpStream, first);

			EmitTraceEventSrType(report);

			return;
		}

		// If not found, check with RTX.
		auto it2 = this->mapRtxSsrcRtpStream.find(report->GetSsrc());

		if (it2 != this->mapRtxSsrcRtpStream.end())
		{
			auto* rtpStream = it2->second;

			rtpStream->ReceiveRtxRtcpSenderReport(report);

			return;
		}

		MS_DEBUG_TAG(rtcp, "RtpStream not found [ssrc:%" PRIu32 "]", report->GetSsrc());
	}

	void Producer::ReceiveRtcpXrDelaySinceLastRr(RTC::RTCP::DelaySinceLastRr::SsrcInfo* ssrcInfo)
	{
		MS_TRACE();

		auto it = this->mapSsrcRtpStream.find(ssrcInfo->GetSsrc());

		if (it == this->mapSsrcRtpStream.end())
		{
			MS_WARN_TAG(rtcp, "RtpStream not found [ssrc:%" PRIu32 "]", ssrcInfo->GetSsrc());

			return;
		}

		auto* rtpStream = it->second;

		rtpStream->ReceiveRtcpXrDelaySinceLastRr(ssrcInfo);
	}

	bool Producer::GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs)
	{
		MS_TRACE();

		if (static_cast<float>((nowMs - this->lastRtcpSentTime) * 1.15) < this->maxRtcpInterval)
		{
			return true;
		}

		std::vector<RTCP::ReceiverReport*> receiverReports;
		RTCP::ReceiverReferenceTime* receiverReferenceTimeReport{ nullptr };

		for (auto& kv : this->mapSsrcRtpStream)
		{
			auto* rtpStream = kv.second;
			auto* report    = rtpStream->GetRtcpReceiverReport();

			receiverReports.push_back(report);

			auto* rtxReport = rtpStream->GetRtxRtcpReceiverReport();

			if (rtxReport)
			{
				receiverReports.push_back(rtxReport);
			}
		}

		// Add a receiver reference time report if no present in the packet.
		if (!packet->HasReceiverReferenceTime())
		{
			auto ntp                    = Utils::Time::TimeMs2Ntp(nowMs);
			receiverReferenceTimeReport = new RTC::RTCP::ReceiverReferenceTime();

			receiverReferenceTimeReport->SetNtpSec(ntp.seconds);
			receiverReferenceTimeReport->SetNtpFrac(ntp.fractions);
		}

		// RTCP Compound packet buffer cannot hold the data.
		if (!packet->Add(receiverReports, receiverReferenceTimeReport))
		{
			return false;
		}

		this->lastRtcpSentTime = nowMs;

		return true;
	}

	void Producer::RequestKeyFrame(uint32_t mappedSsrc, bool fromViewerRtcp, bool firstFrameRequest)
	{
		MS_TRACE();

		if (!this->keyFrameRequestManager || this->paused)
		{
			MS_DEBUG_DEV(
			  "producer key frame request ignored [producerId:%s, mappedSsrc:%" PRIu32
			  ", hasManager:%s, paused:%s]",
			  this->id.c_str(),
			  mappedSsrc,
			  this->keyFrameRequestManager ? "true" : "false",
			  this->paused ? "true" : "false");
			return;
		}

		// Once the SFU enforces its own key frame cadence (keyFrameRequestDelay
		// > 0), viewer RTCP PLI/FIR must not drive publisher key frame
		// generation: many viewers could otherwise keep the publisher
		// generating back-to-back key frames. The cadence watchdog is then the
		// owner of request timing; viewer asks are still counted and reported
		// (rate-limited) so freeze incidents can prove that viewers asked and
		// how long the policy held them back.
		//
		// Exception (2026-09-17 ZL92061/front): a viewer that SimpleConsumer
		// still reports as first-frame unconfirmed (it was handed a key frame
		// that its RTCP Receiver Report never acknowledged) is not a served
		// viewer, so its ask is forwarded through the first-frame path below
		// instead of being counted and dropped here. The guard rails are
		// unchanged: at most one forwarded request per SSRC per viewer
		// force-spacing, coalesced across every waiting consumer.
		if (fromViewerRtcp && this->keyFrameRequestDelay > 0u && !firstFrameRequest)
		{
			++this->suppressedViewerKeyFrameRequests;

			const uint64_t nowMs = DepLibUV::GetTimeMs();

			// Rate-limit to one evidence line per second per producer: a real
			// viewer storm stays diagnosable without flooding the log.  The
			// line must survive default deployments, so it uses the
			// level-gated evidence channel (never logTags).
			if (nowMs - this->lastSuppressedViewerRequestLogAtMs >= 1000u)
			{
				this->lastSuppressedViewerRequestLogAtMs = nowMs;

				MS_EVIDENCE_WARN(
				  "viewer key frame request suppressed [producerId:%s, mappedSsrc:%" PRIu32
				  ", suppressedTotal:%" PRIu64 "]",
				  this->id.c_str(),
				  mappedSsrc,
				  this->suppressedViewerKeyFrameRequests);
			}

			return;
		}

		// Resolve the producer SSRC once; both the first-frame branch and the
		// regular coalesced path need it.
		const uint32_t ssrc = ResolveSsrcFromMappedSsrc(mappedSsrc);

		if (ssrc == 0u)
		{
			MS_WARN_DEV(
			  "producer key frame request mappedSsrc not found [producerId:%s, mappedSsrc:%" PRIu32 "]",
			  this->id.c_str(),
			  mappedSsrc);

			return;
		}

		// Log when the SSRC had to be recovered from the static rtpMapping
		// (the dynamic per-ssrc mapping did not contain it yet).
		if (this->mapMappedSsrcSsrc.find(mappedSsrc) == this->mapMappedSsrcSsrc.end())
		{
			MS_DEBUG_DEV(
			  "producer key frame request recovered ssrc from static rtpMapping [producerId:%s, mappedSsrc:%" PRIu32
			  ", ssrc:%" PRIu32 "]",
			  this->id.c_str(),
			  mappedSsrc,
			  ssrc);
		}

		// If the current RTP packet is a key frame for the given SSRC do
		// nothing since we are gonna provide Consumers with the requested key
		// frame right now.
		//
		// NOTE: We know that this may only happen before calling MangleRtpPacket()
		// so the SSRC of the packet is still the original one and not the mapped one.
		//
		// clang-format off
		if (
			this->currentRtpPacket &&
			this->currentRtpPacket->GetSsrc() == ssrc &&
			this->currentRtpPacket->IsKeyFrame()
		)
		// clang-format on
		{
			MS_DEBUG_DEV(
			  "producer key frame request skipped because current packet is already key frame [producerId:%s, mappedSsrc:%" PRIu32 ", ssrc:%" PRIu32 "]",
			  this->id.c_str(),
			  mappedSsrc,
			  ssrc);
			return;
		}

		// First-frame requests (a consumer that has not yet delivered any key
		// frame) bypass the coalescing delay: a new viewer must not wait out
		// the window before rendering anything.  A per-ssrc force-spacing
		// guard keeps simultaneous joins from turning into a request burst;
		// the pending retry timer covers the remainder. Viewer-originated
		// first-frame asks (an unconfirmed handoff) take the same path with a
		// tighter spacing.
		if (firstFrameRequest)
		{
			HandleFirstFrameKeyFrameRequest(ssrc, fromViewerRtcp);

			return;
		}

		MS_DEBUG_DEV(
		  "producer key frame request scheduled [producerId:%s, mappedSsrc:%" PRIu32 ", ssrc:%" PRIu32 "]",
		  this->id.c_str(),
		  mappedSsrc,
		  ssrc);
		this->keyFrameRequestManager->KeyFrameNeeded(ssrc);
	}

	// Pure three-state decision for a first-frame key frame request (weekly
	// review 2026-09-15).  Fold when the next scheduled release is imminent
	// (it will serve this viewer too); otherwise force now, but never more
	// often than forceSpacingMs per ssrc so a mass join stays a single
	// request.  A missing or already-overdue schedule is FORCE: never make a
	// first viewer wait out the window.
	Producer::FirstFrameAction Producer::DecideFirstFrameAction(
	  uint64_t nextReleaseMs,
	  uint64_t nowMs,
	  uint64_t lastFirstFrameForceAtMs,
	  uint64_t foldWindowMs,
	  uint64_t forceSpacingMs)
	{
		// No baseline (0) or already overdue: force.
		if (nextReleaseMs == 0u || nextReleaseMs <= nowMs)
		{
			return FirstFrameAction::FORCE;
		}

		const uint64_t remainingMs = nextReleaseMs - nowMs;

		if (remainingMs <= foldWindowMs)
		{
			return FirstFrameAction::FOLD;
		}

		if (nowMs - lastFirstFrameForceAtMs >= forceSpacingMs)
		{
			return FirstFrameAction::FORCE;
		}

		return FirstFrameAction::FOLD;
	}

	uint32_t Producer::ResolveSsrcFromMappedSsrc(uint32_t mappedSsrc) const
	{
		MS_TRACE();

		auto it = this->mapMappedSsrcSsrc.find(mappedSsrc);

		if (it != this->mapMappedSsrcSsrc.end())
		{
			return it->second;
		}

		for (const auto& encodingMapping : this->rtpMapping.encodings)
		{
			if (encodingMapping.mappedSsrc == mappedSsrc && encodingMapping.ssrc != 0u)
			{
				return encodingMapping.ssrc;
			}
		}

		return 0u;
	}

	uint64_t Producer::GetNextKeyFrameReleaseEstimateMs(uint32_t ssrc) const
	{
		MS_TRACE();

		uint64_t lastKeyFrameMs{ 0u };
		auto keyFrameIt = this->mapSsrcKeyFrameCadenceAtMs.find(ssrc);

		if (keyFrameIt != this->mapSsrcKeyFrameCadenceAtMs.end())
		{
			lastKeyFrameMs = keyFrameIt->second;
		}

		uint64_t lastRequestMs{ 0u };
		auto requestIt = this->mapSsrcLastKeyFrameRequestAtMs.find(ssrc);

		if (requestIt != this->mapSsrcLastKeyFrameRequestAtMs.end())
		{
			lastRequestMs = requestIt->second;
		}

		const uint64_t lastEventMs = lastKeyFrameMs > lastRequestMs ? lastKeyFrameMs : lastRequestMs;

		// No baseline yet: the caller treats 0 as "no schedule to rely on".
		if (lastEventMs == 0u)
		{
			return 0u;
		}

		return lastEventMs + this->keyFrameRequestDelay;
	}

	void Producer::HandleFirstFrameKeyFrameRequest(uint32_t ssrc, bool fromViewerRtcp)
	{
		MS_TRACE();

		const uint64_t nowMs = DepLibUV::GetTimeMs();

		// The fold window must not swallow the whole cadence: when the delay
		// is small, only requests within half a cadence of the next release
		// are folded and the rest are forced (never make a first viewer wait).
		const uint64_t foldWindowMs =
		  std::min<uint64_t>(KeyFrameFirstFrameFoldWindowMs, this->keyFrameRequestDelay / 2u);

		auto lastForceIt = this->mapSsrcLastFirstFrameForceAtMs.find(ssrc);

		const uint64_t lastForceAtMs =
		  lastForceIt != this->mapSsrcLastFirstFrameForceAtMs.end() ? lastForceIt->second : 0u;

		const FirstFrameAction action = DecideFirstFrameAction(
		  GetNextKeyFrameReleaseEstimateMs(ssrc),
		  nowMs,
		  lastForceAtMs,
		  foldWindowMs,
		  fromViewerRtcp ? KeyFrameFirstFrameViewerSpacingMs : KeyFrameFirstFrameRequestSpacingMs);

		if (action == FirstFrameAction::FORCE)
		{
			++this->firstFrameForced;
			this->mapSsrcLastFirstFrameForceAtMs[ssrc] = nowMs;

			MS_DEBUG_DEV(
			  "producer first-frame key frame request bypassing coalescing delay [producerId:%s, ssrc:%" PRIu32
			  "]",
			  this->id.c_str(),
			  ssrc);

			this->keyFrameRequestManager->ForceKeyFrameNeeded(ssrc);
		}
		else
		{
			++this->firstFrameFolded;

			MS_DEBUG_DEV(
			  "producer first-frame key frame request folded into scheduled release [producerId:%s, ssrc:%" PRIu32
			  "]",
			  this->id.c_str(),
			  ssrc);

			this->keyFrameRequestManager->KeyFrameNeeded(ssrc);
		}

		// Rate-limited production evidence (same channel as viewer suppression
		// so it survives default deployments): proves the policy ran and how
		// often a first viewer was forced vs folded.
		if (nowMs - this->lastFirstFrameEvidenceLogAtMs >= 1000u)
		{
			this->lastFirstFrameEvidenceLogAtMs = nowMs;

			MS_EVIDENCE_WARN(
			  "producer first-frame key frame request %s [producerId:%s, ssrc:%" PRIu32
			  ", source:%s, forcedTotal:%" PRIu64 ", foldedTotal:%" PRIu64 "]",
			  action == FirstFrameAction::FORCE ? "forced" : "folded",
			  this->id.c_str(),
			  ssrc,
			  fromViewerRtcp ? "viewer-rtcp" : "internal",
			  this->firstFrameForced,
			  this->firstFrameFolded);
		}
	}

	void Producer::MarkKeyFrameCadenceBaseline(uint32_t ssrc, uint64_t nowMs)
	{
		this->mapSsrcKeyFrameCadenceAtMs[ssrc] = nowMs;
	}

	void Producer::MarkKeyFrameRequestSent(uint32_t ssrc, uint64_t nowMs)
	{
		this->mapSsrcLastKeyFrameRequestAtMs[ssrc] = nowMs;
	}

	void Producer::CheckKeyFrameCadence(uint32_t ssrc, uint64_t nowMs)
	{
		MS_TRACE();

		// Lazy key frame cadence watchdog: evaluated on every non-key-frame media
		// packet, so a continuously streaming publisher is checked at packet
		// cadence without a resident timer (a resident timer would also hang
		// UV_RUN_DEFAULT based unit tests). This is a best-effort, packet-driven
		// check: it promises neither a periodic timer nor a decodable key frame;
		// it only requests one key frame when, while media keeps flowing, no key
		// frame has been seen for longer than keyFrameRequestDelay.
		if (this->keyFrameRequestDelay == 0u || this->paused || !this->keyFrameRequestManager)
		{
			return;
		}

		// Fire only when the next scheduled release (max(lastKeyFrame,
		// lastRequest) + delay) is already due; 0 means no baseline yet (the
		// watchdog must not fire before the first key frame).  This is the same
		// reasoning the first-frame early pass uses, so both share
		// GetNextKeyFrameReleaseEstimateMs() instead of duplicating it.
		//
		// This enforces a minimum spacing against ANY previous forwarded
		// request (new stream, resume, coalesced internal request, earlier
		// watchdog fire, NACK-generator recovery via the manager), not just
		// against watchdog fires. Without this, an internal request at
		// t=interval-epsilon would be immediately followed by another watchdog
		// request.
		//
		// Exact guarantee and remaining exceptions:
		// - Watchdog fires are spaced at least keyFrameRequestDelay from both
		//   the last key frame and the last forwarded request.
		// - Exception 1 (emergency, by design): new-stream and resume forced
		//   requests bypass the spacing.
		// - Exception 2 (lost-feedback recovery, upstream behavior): the
		//   manager's pending request performs one bounded retry ~1s after a
		//   request whose key frame has not arrived, without the spacing check.
		// Full unification of every path into one scheduler is the next design
		// packet (change folder T15).
		const uint64_t nextReleaseMs = GetNextKeyFrameReleaseEstimateMs(ssrc);

		if (nextReleaseMs == 0u || nextReleaseMs > nowMs)
		{
			return;
		}

		uint64_t sinceLastKeyFrameMs{ 0u };
		auto keyFrameIt = this->mapSsrcKeyFrameCadenceAtMs.find(ssrc);

		if (keyFrameIt != this->mapSsrcKeyFrameCadenceAtMs.end())
		{
			sinceLastKeyFrameMs = nowMs - keyFrameIt->second;
		}

		// Refresh the baseline before requesting so a packet burst while waiting
		// for the publisher's key frame triggers exactly one request per interval.
		MarkKeyFrameCadenceBaseline(ssrc, nowMs);

		MS_DEBUG_2TAGS(
		  rtp, rtcp,
		  "key frame cadence watchdog requesting key frame [ssrc:%" PRIu32
		  ", intervalMs:%" PRIu32 ", sinceLastKeyFrameMs:%" PRIu64
		  ", suppressedViewerRequests:%" PRIu64 "]",
		  ssrc,
		  this->keyFrameRequestDelay,
		  sinceLastKeyFrameMs,
		  this->suppressedViewerKeyFrameRequests);

		this->keyFrameRequestManager->ForceKeyFrameNeeded(ssrc);
	}

	Producer::KeyFrameTrackResult Producer::TrackUpstreamKeyFramePacket(
	  RTC::RtpPacket* packet, bool isRtx, uint64_t nowMs)
	{
		MS_TRACE();

		const uint32_t ssrc = packet->GetSsrc();
		const bool credibleFrameStart = packet->IsKeyFrame() && packet->IsFrameStart() &&
		                                !this->keyFrameStartDisabledSsrcs.contains(ssrc);

		// A credible start for a new timestamp creates a new candidate without
		// finalizing older candidates: RTX for the old timestamp may still arrive
		// within its bounded recovery window.
		if (credibleFrameStart)
		{
			auto ssrcIt = this->mapSsrcKeyFrameCandidates.find(ssrc);
			if (ssrcIt == this->mapSsrcKeyFrameCandidates.end() ||
			    ssrcIt->second.find(packet->GetTimestamp()) == ssrcIt->second.end())
			{
				StartKeyFrameCandidate(packet, nowMs);
			}
			else
			{
				auto candidateIt = ssrcIt->second.find(packet->GetTimestamp());
				const bool duplicateSeq =
				  candidateIt->second->receivedSeqs.count(packet->GetSequenceNumber()) != 0u;
				// Two distinct slice-header first-slice markers within one picture
				// are impossible in a valid H.265 stream and indicate H.264 FMO
				// (one first_mb==0 slice per slice group) or a corrupt stream.
				// The frame-start evidence for this SSRC is untrustworthy: drop
				// its candidates and stop starting new ones.  Codec descriptor
				// starts (e.g. VP9 per-layer start bits) may legitimately repeat
				// within one picture and are excluded.  An RTX redelivery of an
				// already recorded sequence number is a harmless duplicate.
				if (packet->IsFrameStartFromSliceHeader() && !isRtx && !duplicateSeq)
				{
					MS_EVIDENCE_WARN(
					  "upstream key frame start conflict, first-slice heuristic disabled for stream [ssrc:%" PRIu32
					  ", timestamp:%" PRIu32 ", startSeq:%" PRIu16 "]",
					  ssrc,
					  packet->GetTimestamp(),
					  packet->GetSequenceNumber());
					this->keyFrameStartDisabledSsrcs.insert(ssrc);
					StartKeyFrameEvidenceTimerIfNeeded();
					ClearKeyFrameCandidate(ssrc, "frame_start_conflict", /*requestRecovery=*/false);

					return KeyFrameTrackResult::IGNORED;
				}
			}
		}

		auto ssrcIt = this->mapSsrcKeyFrameCandidates.find(ssrc);
		if (ssrcIt == this->mapSsrcKeyFrameCandidates.end())
		{
			// A frame end for a picture with key-frame NAL traffic but no
			// candidate means the credible start (first slice / start packet)
			// never arrived.  FU-fragmented frames split IsKeyFrame and the
			// marker across different packets, so the decision uses the
			// per-timestamp history instead of the current packet alone.
			if (packet->HasMarker() && SawKeyFrameTrafficForTimestamp(ssrc, packet->GetTimestamp()))
			{
				WarnKeyFrameEndWithoutStart(ssrc, packet->GetTimestamp(), packet->GetSequenceNumber(), nowMs);
			}

			return KeyFrameTrackResult::IGNORED;
		}

		auto candidateIt = ssrcIt->second.find(packet->GetTimestamp());
		if (candidateIt == ssrcIt->second.end())
		{
			// No candidate for this timestamp.  Leave older candidates active and
			// let the cadence watchdog inspect ordinary non-key-frame traffic.
			if (packet->HasMarker() && SawKeyFrameTrafficForTimestamp(ssrc, packet->GetTimestamp()))
			{
				WarnKeyFrameEndWithoutStart(ssrc, packet->GetTimestamp(), packet->GetSequenceNumber(), nowMs);
			}

			return KeyFrameTrackResult::IGNORED;
		}

		auto* candidate = candidateIt->second;
		const bool newlyInserted = candidate->receivedSeqs.insert(packet->GetSequenceNumber()).second;
		if (newlyInserted && isRtx)
		{
			++candidate->repairedPackets;
		}

		if (packet->HasMarker())
		{
			candidate->markerSeen = true;
			candidate->markerSeq  = packet->GetSequenceNumber();
		}

		if (packet->IsFrameEnd(packet->HasMarker()))
		{
			candidate->hasEnd = true;
			candidate->endSeq = packet->GetSequenceNumber();
		}

		if (KeyFrameCandidateIsComplete(*candidate))
		{
			FinalizeKeyFrameCandidate(
			  ssrc, candidate, "complete", /*complete=*/true, /*requestRecovery=*/false, nowMs);
			return KeyFrameTrackResult::FINALIZED;
		}

		return KeyFrameTrackResult::ACTIVE;
	}

	void Producer::StartKeyFrameCandidate(RTC::RtpPacket* packet, uint64_t nowMs)
	{
		MS_TRACE();

		const uint32_t ssrc = packet->GetSsrc();
		auto* candidate = new KeyFrameCandidate();
		candidate->timestamp   = packet->GetTimestamp();
		candidate->startSeq    = packet->GetSequenceNumber();
		candidate->startedAtMs = nowMs;
		candidate->timer       = new TimerHandle(this);
		candidate->timer->Start(KeyFrameCandidateTimeoutMs);

		// Recover same-timestamp packets that arrived before the credible frame
		// start (misorder or an RTX whose original start packet arrives later).
		auto historyIt = this->mapSsrcKeyFramePacketHistory.find(ssrc);
		if (historyIt != this->mapSsrcKeyFramePacketHistory.end())
		{
			for (const auto& kv : historyIt->second.packets)
			{
				if (kv.second.timestamp != candidate->timestamp ||
				    RTC::SeqManager<uint16_t>::IsSeqLowerThan(kv.first, candidate->startSeq))
				{
					continue;
				}

				candidate->receivedSeqs.insert(kv.first);
				if (kv.second.frameEnd)
				{
					candidate->hasEnd = true;
					candidate->endSeq = kv.first;
				}
				if (kv.second.marker)
				{
					candidate->markerSeen = true;
					candidate->markerSeq = kv.first;
				}
			}
		}

		auto latestIt = this->mapSsrcLatestKeyFrameStartedTimestamp.find(ssrc);
		if (latestIt == this->mapSsrcLatestKeyFrameStartedTimestamp.end() ||
		    RTC::SeqManager<uint32_t>::IsSeqHigherThan(candidate->timestamp, latestIt->second))
		{
			this->mapSsrcLatestKeyFrameStartedTimestamp[ssrc] = candidate->timestamp;
		}
		this->mapSsrcKeyFrameCandidates[ssrc][candidate->timestamp] = candidate;

		MS_DEBUG_TAG(
		  rtp,
		  "upstream key frame candidate started [ssrc:%" PRIu32
		  ", timestamp:%" PRIu32 ", startSeq:%" PRIu16 ", timeoutMs:%" PRIu64 "]",
		  ssrc,
		  candidate->timestamp,
		  candidate->startSeq,
		  KeyFrameCandidateTimeoutMs);
	}

	void Producer::FinalizeKeyFrameCandidate(
	  uint32_t ssrc,
	  KeyFrameCandidate* candidate,
	  const char* reason,
	  bool complete,
	  bool requestRecovery,
	  uint64_t nowMs)
	{
		MS_TRACE();

		const uint32_t timestamp = candidate->timestamp;
		const uint16_t startSeq = candidate->startSeq;
		const uint16_t endSeq = candidate->hasEnd ? candidate->endSeq : candidate->startSeq;
		const bool hasEnd = candidate->hasEnd;
		const size_t missingPackets = hasEnd ? CountMissingKeyFramePackets(*candidate) : 0u;
		const size_t repairedPackets = candidate->repairedPackets;
		const uint64_t waitedMs = nowMs - candidate->startedAtMs;
		// Capture the bounded missing-sequence list while the candidate is
		// still alive; it is emitted with the incomplete evidence below.
		std::string missingSeqList;
		size_t listedMissing{ 0u };
		if (hasEnd)
		{
			for (uint16_t seq = candidate->startSeq;
			     seq != candidate->endSeq && listedMissing < KeyFrameMissingSeqListMax;
			     ++seq)
			{
				if (candidate->receivedSeqs.count(seq) == 0u)
				{
					if (!missingSeqList.empty())
					{
						missingSeqList += ",";
					}
					missingSeqList += std::to_string(seq);
					++listedMissing;
				}
			}
			if (
				listedMissing < KeyFrameMissingSeqListMax &&
				candidate->receivedSeqs.count(candidate->endSeq) == 0u)
			{
				if (!missingSeqList.empty())
				{
					missingSeqList += ",";
				}
				missingSeqList += std::to_string(candidate->endSeq);
				++listedMissing;
			}
		}

		auto latestStartedIt = this->mapSsrcLatestKeyFrameStartedTimestamp.find(ssrc);
		const bool isLatestStarted =
		  latestStartedIt != this->mapSsrcLatestKeyFrameStartedTimestamp.end() &&
		  latestStartedIt->second == timestamp;
		auto ssrcIt = this->mapSsrcKeyFrameCandidates.find(ssrc);

		candidate->timer->Stop();
		delete candidate->timer;
		candidate->timer = nullptr;

		if (ssrcIt != this->mapSsrcKeyFrameCandidates.end())
		{
			ssrcIt->second.erase(timestamp);
			if (ssrcIt->second.empty())
			{
				this->mapSsrcKeyFrameCandidates.erase(ssrcIt);
			}
		}
		delete candidate;

		if (complete)
		{
			++this->mapSsrcCompleteKeyFrames[ssrc];
			this->mapSsrcLastKeyFrameCompleteAtMs[ssrc] = nowMs;
			this->ssrcsSeenCompleteKeyFrame.insert(ssrc);
			StartKeyFrameEvidenceTimerIfNeeded();

			// Only the newest started candidate may clear pending state or refresh
			// the cadence baseline.  An older candidate completing after a newer
			// frame started is evidence, but it must not override the newer frame.
			// Enforcement is gated on cadence mode exactly like the original
			// reachability: observation must not add or remove scheduling
			// effects (legacy mode keeps its per-key-frame handling above).
			if (isLatestStarted && this->keyFrameRequestDelay > 0u && this->keyFrameRequestManager)
			{
				this->keyFrameRequestManager->KeyFrameReceived(ssrc);
				MarkKeyFrameCadenceBaseline(ssrc, nowMs);
			}

			MS_DEBUG_TAG(
			  rtp,
			  "upstream key frame complete [ssrc:%" PRIu32
			  ", timestamp:%" PRIu32 ", startSeq:%" PRIu16 ", endSeq:%" PRIu16
			  ", repairedPackets:%zu, waitedMs:%" PRIu64 ", latest:%s]",
			  ssrc,
			  timestamp,
			  startSeq,
			  endSeq,
			  repairedPackets,
			  waitedMs,
			  isLatestStarted ? "true" : "false");
			(void)requestRecovery;

			MaybeLogKeyFrameSummary(ssrc, nowMs);

			return;
		}

		++this->mapSsrcIncompleteKeyFrames[ssrc];
		this->mapSsrcLastKeyFrameIncompleteAtMs[ssrc] = nowMs;
		StartKeyFrameEvidenceTimerIfNeeded();

		if (std::string_view(reason) == "timeout")
		{
			MS_EVIDENCE_WARN(
			  "upstream key frame incomplete [ssrc:%" PRIu32
			  ", timestamp:%" PRIu32 ", startSeq:%" PRIu16 ", endSeq:%" PRIu16
			  ", hasEnd:%s, missingPackets:%zu, missingSeqs:%s, repairedPackets:%zu, waitedMs:%" PRIu64
			  ", latest:%s, reason:%s]",
			  ssrc,
			  timestamp,
			  startSeq,
			  endSeq,
			  hasEnd ? "true" : "false",
			  missingPackets,
			  missingSeqList.empty() ? "-" : missingSeqList.c_str(),
			  repairedPackets,
			  waitedMs,
			  isLatestStarted ? "true" : "false",
			  reason);
		}
		else
		{
			MS_DEBUG_2TAGS(
			  rtp, rtcp,
			  "upstream key frame incomplete [ssrc:%" PRIu32
			  ", timestamp:%" PRIu32 ", startSeq:%" PRIu16 ", endSeq:%" PRIu16
			  ", hasEnd:%s, missingPackets:%zu, missingSeqs:%s, repairedPackets:%zu, waitedMs:%" PRIu64
			  ", latest:%s, reason:%s]",
			  ssrc,
			  timestamp,
			  startSeq,
			  endSeq,
			  hasEnd ? "true" : "false",
			  missingPackets,
			  missingSeqList.empty() ? "-" : missingSeqList.c_str(),
			  repairedPackets,
			  waitedMs,
			  isLatestStarted ? "true" : "false",
			  reason);
		}

		MaybeLogKeyFrameSummary(ssrc, nowMs);

		// Only the newest candidate may request recovery.  Old candidates may
		// still time out after a newer frame started; they must not trigger an
		// additional publisher request.  Cadence-mode gating preserves the
		// original scheduling; legacy observation never requests.
		if (isLatestStarted && requestRecovery && this->keyFrameRequestDelay > 0u && this->keyFrameRequestManager)
		{
			this->keyFrameRequestManager->KeyFrameNeeded(ssrc);
		}
	}

	bool Producer::KeyFrameCandidateIsComplete(const KeyFrameCandidate& candidate) const
	{
		if (!candidate.hasEnd)
		{
			return false;
		}

		// A marker on another packet contradicts the codec-parsed end.  This can
		// happen with layer/frame-marking mismatches; treat it as unknown.
		if (candidate.markerSeen && candidate.markerSeq != candidate.endSeq)
		{
			return false;
		}

		const uint16_t span = static_cast<uint16_t>(candidate.endSeq - candidate.startSeq);
		if (span > KeyFrameMaxSequenceSpan)
		{
			return false;
		}

		for (uint16_t seq = candidate.startSeq; seq != candidate.endSeq; ++seq)
		{
			if (candidate.receivedSeqs.count(seq) == 0u)
			{
				return false;
			}
		}

		return candidate.receivedSeqs.count(candidate.endSeq) != 0u;
	}

	void Producer::ResetKeyFrameGenerationState(uint32_t ssrc)
	{
		this->mapSsrcKeyFramePacketHistory.erase(ssrc);
		this->mapSsrcKeyFrameFirstReceived.erase(ssrc);
		this->keyFrameStartDisabledSsrcs.erase(ssrc);
		this->ssrcsSeenCompleteKeyFrame.erase(ssrc);
		this->mapSsrcNoStartWarningCount.erase(ssrc);
		this->mapSsrcLastNoStartInfoClassified.erase(ssrc);
		this->mapSsrcLastKeyFrameNoStartWarnAtMs.erase(ssrc);
	}

	void Producer::StartKeyFrameEvidenceTimerIfNeeded()
	{
		if (
		  this->keyFrameEvidenceTimerClosed || this->keyFrameEvidenceTimer != nullptr ||
		  this->keyFrameEvidenceFlushIntervalMs == 0u)
		{
			return;
		}

		this->keyFrameEvidenceTimer = new TimerHandle(this);
		this->keyFrameEvidenceTimer->Start(
		  this->keyFrameEvidenceFlushIntervalMs, this->keyFrameEvidenceFlushIntervalMs);
	}

	void Producer::StopKeyFrameEvidenceTimer() noexcept
	{
		if (this->keyFrameEvidenceTimer == nullptr)
		{
			return;
		}

		this->keyFrameEvidenceTimer->Stop();
		delete this->keyFrameEvidenceTimer;
		this->keyFrameEvidenceTimer = nullptr;
	}

	Producer::KeyFrameSummarySnapshot Producer::CurrentKeyFrameSummarySnapshot(uint32_t ssrc) const
	{
		KeyFrameSummarySnapshot snapshot;
		snapshot.complete = this->mapSsrcCompleteKeyFrames.count(ssrc) != 0u
		                      ? this->mapSsrcCompleteKeyFrames.at(ssrc)
		                      : 0u;
		snapshot.incomplete = this->mapSsrcIncompleteKeyFrames.count(ssrc) != 0u
		                        ? this->mapSsrcIncompleteKeyFrames.at(ssrc)
		                        : 0u;
		snapshot.lastCompleteAtMs = this->mapSsrcLastKeyFrameCompleteAtMs.count(ssrc) != 0u
		                              ? this->mapSsrcLastKeyFrameCompleteAtMs.at(ssrc)
		                              : 0u;
		snapshot.lastIncompleteAtMs = this->mapSsrcLastKeyFrameIncompleteAtMs.count(ssrc) != 0u
		                                ? this->mapSsrcLastKeyFrameIncompleteAtMs.at(ssrc)
		                                : 0u;
		return snapshot;
	}

	bool Producer::KeyFrameEvidenceDirty(uint32_t ssrc) const
	{
		auto it = this->mapSsrcKeyFrameSummarySnapshot.find(ssrc);
		if (it == this->mapSsrcKeyFrameSummarySnapshot.end())
		{
			return true;
		}

		return !(it->second == CurrentKeyFrameSummarySnapshot(ssrc));
	}

	void Producer::CollectKeyFrameEvidenceSsrcs(absl::flat_hash_set<uint32_t>& ssrcs) const
	{
		for (const auto& kv : this->mapSsrcCompleteKeyFrames)
		{
			ssrcs.insert(kv.first);
		}
		for (const auto& kv : this->mapSsrcIncompleteKeyFrames)
		{
			ssrcs.insert(kv.first);
		}
		for (const auto ssrc : this->keyFrameStartDisabledSsrcs)
		{
			ssrcs.insert(ssrc);
		}
	}

	void Producer::FlushDirtyKeyFrameSummaries(uint64_t nowMs)
	{
		absl::flat_hash_set<uint32_t> evidenceSsrcs;
		CollectKeyFrameEvidenceSsrcs(evidenceSsrcs);

		bool stillDirty{ false };
		for (const auto ssrc : evidenceSsrcs)
		{
			if (!KeyFrameEvidenceDirty(ssrc))
			{
				continue;
			}

			MaybeLogKeyFrameSummary(ssrc, nowMs, /*force=*/true);
			// Re-check after the flush: emitting updates the snapshot, but a
			// concurrent producer-close path could have added new evidence.
			if (KeyFrameEvidenceDirty(ssrc))
			{
				stillDirty = true;
			}
		}

		// Nothing new remains: stop the timer until new evidence arrives, so
		// idle producers do not keep a resident timer.
		if (!stillDirty)
		{
			StopKeyFrameEvidenceTimer();
		}
	}

	void Producer::MaybeLogKeyFrameSummary(uint32_t ssrc, uint64_t nowMs, bool force)
	{
		if (!force)
		{
			auto it = this->mapSsrcLastKeyFrameSummaryAtMs.find(ssrc);
			if (it != this->mapSsrcLastKeyFrameSummaryAtMs.end() &&
			    nowMs - it->second < KeyFrameSummaryIntervalMs)
			{
				return;
			}
		}

		const uint64_t complete =
		  this->mapSsrcCompleteKeyFrames.count(ssrc) != 0u ? this->mapSsrcCompleteKeyFrames[ssrc] : 0u;
		const uint64_t incomplete =
		  this->mapSsrcIncompleteKeyFrames.count(ssrc) != 0u ? this->mapSsrcIncompleteKeyFrames[ssrc] : 0u;
		const bool heuristicDisabled = this->keyFrameStartDisabledSsrcs.contains(ssrc);

		// Only emit once there is something to report; an idle stream should
		// not produce periodic empty summaries.
		if (complete == 0u && incomplete == 0u && !heuristicDisabled)
		{
			return;
		}

		this->mapSsrcLastKeyFrameSummaryAtMs[ssrc] = nowMs;
		this->mapSsrcKeyFrameSummarySnapshot[ssrc] = CurrentKeyFrameSummarySnapshot(ssrc);
		++this->keyFrameSummaryEmissions;

		auto completeIt = this->mapSsrcLastKeyFrameCompleteAtMs.find(ssrc);
		auto incompleteIt = this->mapSsrcLastKeyFrameIncompleteAtMs.find(ssrc);
		const uint64_t lastCompleteAtMs =
		  completeIt != this->mapSsrcLastKeyFrameCompleteAtMs.end() ? completeIt->second : 0u;
		const uint64_t lastIncompleteAtMs =
		  incompleteIt != this->mapSsrcLastKeyFrameIncompleteAtMs.end() ? incompleteIt->second : 0u;

		MS_EVIDENCE_INFO(
		  "upstream key frame integrity summary [ssrc:%" PRIu32
		  ", complete:%" PRIu64 ", incomplete:%" PRIu64
		  ", lastCompleteAtMs:%" PRIu64 ", lastCompleteAgeMs:%" PRIu64
		  ", lastIncompleteAtMs:%" PRIu64 ", lastIncompleteAgeMs:%" PRIu64
		  ", firstSliceHeuristic:%s, mode:%s, reason:%s]",
		  ssrc,
		  complete,
		  incomplete,
		  lastCompleteAtMs,
		  lastCompleteAtMs == 0u ? 0u : nowMs - lastCompleteAtMs,
		  lastIncompleteAtMs,
		  lastIncompleteAtMs == 0u ? 0u : nowMs - lastIncompleteAtMs,
		  heuristicDisabled ? "disabled" : "active",
		  this->keyFrameIntegrityObserve ? "observe" : "cadence",
		  force ? "forced" : "interval");
	}

	bool Producer::SawKeyFrameTrafficForTimestamp(uint32_t ssrc, uint32_t timestamp) const
	{
		auto historyIt = this->mapSsrcKeyFramePacketHistory.find(ssrc);
		if (historyIt == this->mapSsrcKeyFramePacketHistory.end())
		{
			return false;
		}

		for (const auto& kv : historyIt->second.packets)
		{
			if (kv.second.timestamp == timestamp && kv.second.keyFrameTraffic)
			{
				return true;
			}
		}

		return false;
	}

	void Producer::WarnKeyFrameEndWithoutStart(uint32_t ssrc, uint32_t timestamp, uint16_t seq, uint64_t nowMs)
	{
		auto it = this->mapSsrcLastKeyFrameNoStartWarnAtMs.find(ssrc);
		if (it != this->mapSsrcLastKeyFrameNoStartWarnAtMs.end() &&
		    nowMs - it->second < this->keyFrameNoStartWarnIntervalMs)
		{
			return;
		}

		this->mapSsrcLastKeyFrameNoStartWarnAtMs[ssrc] = nowMs;

		// Factual context only; interpretation is left to the reader.
		auto firstIt = this->mapSsrcKeyFrameFirstReceived.find(ssrc);
		const uint16_t firstSeq  = firstIt != this->mapSsrcKeyFrameFirstReceived.end() ? firstIt->second.firstSeq : 0u;
		const uint64_t firstAtMs = firstIt != this->mapSsrcKeyFrameFirstReceived.end() ? firstIt->second.firstAtMs : 0u;
		const uint64_t completeSoFar =
		  this->mapSsrcCompleteKeyFrames.count(ssrc) != 0u ? this->mapSsrcCompleteKeyFrames[ssrc] : 0u;
		const uint64_t noStartWarnings = ++this->mapSsrcNoStartWarningCount[ssrc];
		// Phase uses the per-generation flag, not the lifetime counter: a
		// rebuilt stream must classify as pre-first-complete until its own
		// first complete key frame even if the previous generation completed
		// many.
		const bool seenCompleteThisGeneration = this->ssrcsSeenCompleteKeyFrame.contains(ssrc);

		// Phase classification (a phase, NOT a harm verdict):
		//   - pre-first-complete: no complete key frame has been observed on
		//     this stream yet.  Consistent with connection-establishment
		//     truncation, but persistent start loss is also possible.
		//   - established: at least one complete key frame was observed; a
		//     missing start on an established stream is a loss candidate.
		// Repeated pre-first-complete warnings escalate to WARN because a
		// stream that never completes a key frame is anomalous even if each
		// event individually resembles truncation.
		const bool preFirstComplete = !seenCompleteThisGeneration;
		const bool escalated        = preFirstComplete && noStartWarnings >= 3u;
		const bool infoClassified   = preFirstComplete && !escalated;
		this->mapSsrcLastNoStartInfoClassified[ssrc] = infoClassified;

		if (infoClassified)
		{
			MS_EVIDENCE_INFO(
			  "upstream key frame ended without a credible start [ssrc:%" PRIu32
			  ", timestamp:%" PRIu32 ", endSeq:%" PRIu16
			  ", firstReceivedSeq:%" PRIu16 ", firstReceivedAgeMs:%" PRIu64
			  ", completeKeyFrames:%" PRIu64 ", noStartWarnings:%" PRIu64
			  ", firstSliceHeuristic:%s, phase:pre-first-complete, "
			  "note:phase-not-a-harm-verdict]",
			  ssrc,
			  timestamp,
			  seq,
			  firstSeq,
			  firstAtMs == 0u ? 0u : nowMs - firstAtMs,
			  completeSoFar,
			  noStartWarnings,
			  this->keyFrameStartDisabledSsrcs.contains(ssrc) ? "disabled" : "active");
		}
		else
		{
			MS_EVIDENCE_WARN(
			  "upstream key frame ended without a credible start [ssrc:%" PRIu32
			  ", timestamp:%" PRIu32 ", endSeq:%" PRIu16
			  ", firstReceivedSeq:%" PRIu16 ", firstReceivedAgeMs:%" PRIu64
			  ", completeKeyFrames:%" PRIu64 ", noStartWarnings:%" PRIu64
			  ", firstSliceHeuristic:%s, phase:%s]",
			  ssrc,
			  timestamp,
			  seq,
			  firstSeq,
			  firstAtMs == 0u ? 0u : nowMs - firstAtMs,
			  completeSoFar,
			  noStartWarnings,
			  this->keyFrameStartDisabledSsrcs.contains(ssrc) ? "disabled" : "active",
			  escalated ? "pre-first-complete-escalated" : "established");
		}
	}

	size_t Producer::CountMissingKeyFramePackets(const KeyFrameCandidate& candidate) const
	{
		if (!candidate.hasEnd)
		{
			return 0u;
		}

		const uint16_t span = static_cast<uint16_t>(candidate.endSeq - candidate.startSeq);
		if (span > KeyFrameMaxSequenceSpan)
		{
			return static_cast<size_t>(span) + 1u;
		}

		size_t missing{ 0u };
		for (uint16_t seq = candidate.startSeq; seq != candidate.endSeq; ++seq)
		{
			if (candidate.receivedSeqs.count(seq) == 0u)
			{
				++missing;
			}
		}
		if (candidate.receivedSeqs.count(candidate.endSeq) == 0u)
		{
			++missing;
		}

		return missing;
	}

	void Producer::RecordKeyFramePacketHistory(RTC::RtpPacket* packet)
	{
		MS_TRACE();

		const uint32_t ssrc = packet->GetSsrc();
		if (this->mapSsrcKeyFrameFirstReceived.find(ssrc) == this->mapSsrcKeyFrameFirstReceived.end())
		{
			KeyFrameFirstReceived first;
			first.firstSeq  = packet->GetSequenceNumber();
			first.firstAtMs = DepLibUV::GetTimeMs();
			this->mapSsrcKeyFrameFirstReceived[ssrc] = first;
		}

		auto& history = this->mapSsrcKeyFramePacketHistory[packet->GetSsrc()];
		KeyFrameHistoryPacket historyPacket;
		historyPacket.timestamp       = packet->GetTimestamp();
		historyPacket.frameEnd        = packet->IsFrameEnd(packet->HasMarker());
		historyPacket.marker          = packet->HasMarker();
		historyPacket.keyFrameTraffic = packet->IsKeyFrameNal();
		history.packets[packet->GetSequenceNumber()] = historyPacket;

		if (!history.started || RTC::SeqManager<uint16_t>::IsSeqHigherThan(packet->GetSequenceNumber(), history.newestSeq))
		{
			history.started  = true;
			history.newestSeq = packet->GetSequenceNumber();
		}

		if (history.packets.size() <= KeyFrameHistoryMaxPackets)
		{
			return;
		}

		const uint16_t cutoff = static_cast<uint16_t>(history.newestSeq - (KeyFrameHistoryMaxPackets / 2u));
		std::vector<uint16_t> staleSeqs;
		for (const auto& kv : history.packets)
		{
			if (RTC::SeqManager<uint16_t>::IsSeqLowerThan(kv.first, cutoff))
			{
				staleSeqs.push_back(kv.first);
			}
		}
		for (auto seq : staleSeqs)
		{
			history.packets.erase(seq);
		}
	}

	void Producer::ClearKeyFrameCandidate(
	  uint32_t ssrc, const char* reason, bool requestRecovery)
	{
		MS_TRACE();

		auto it = this->mapSsrcKeyFrameCandidates.find(ssrc);
		if (it == this->mapSsrcKeyFrameCandidates.end())
		{
			return;
		}

		std::vector<KeyFrameCandidate*> candidates;
		candidates.reserve(it->second.size());
		for (auto& kv : it->second)
		{
			candidates.push_back(kv.second);
		}

		for (auto* candidate : candidates)
		{
			FinalizeKeyFrameCandidate(
			  ssrc, candidate, reason, /*complete=*/false, requestRecovery, DepLibUV::GetTimeMs());
		}
		this->mapSsrcLatestKeyFrameStartedTimestamp.erase(ssrc);
	}

	void Producer::ClearKeyFrameCandidates(const char* reason, bool requestRecovery)
	{
		MS_TRACE();

		std::vector<uint32_t> ssrcs;
		ssrcs.reserve(this->mapSsrcKeyFrameCandidates.size());
		for (const auto& kv : this->mapSsrcKeyFrameCandidates)
		{
			ssrcs.push_back(kv.first);
		}

		for (auto ssrc : ssrcs)
		{
			ClearKeyFrameCandidate(ssrc, reason, requestRecovery);
		}
	}

	RTC::RtpStreamRecv* Producer::GetRtpStream(RTC::RtpPacket* packet)
	{
		MS_TRACE();

		const uint32_t ssrc       = packet->GetSsrc();
		const uint8_t payloadType = packet->GetPayloadType();

		// If stream found in media ssrcs map, return it.
		{
			auto it = this->mapSsrcRtpStream.find(ssrc);

			if (it != this->mapSsrcRtpStream.end())
			{
				auto* rtpStream = it->second;

				return rtpStream;
			}
		}

		// If stream found in RTX ssrcs map, return it.
		{
			auto it = this->mapRtxSsrcRtpStream.find(ssrc);

			if (it != this->mapRtxSsrcRtpStream.end())
			{
				auto* rtpStream = it->second;

				return rtpStream;
			}
		}

		// Otherwise check our encodings and, if appropriate, create a new stream.

		// First, look for an encoding with matching media or RTX ssrc value.
		for (size_t i{ 0 }; i < this->rtpParameters.encodings.size(); ++i)
		{
			auto& encoding           = this->rtpParameters.encodings[i];
			const auto* mediaCodec   = this->rtpParameters.GetCodecForEncoding(encoding);
			const auto* rtxCodec     = this->rtpParameters.GetRtxCodecForEncoding(encoding);
			const bool isMediaPacket = (mediaCodec->payloadType == payloadType);
			const bool isRtxPacket   = (rtxCodec && rtxCodec->payloadType == payloadType);

			if (isMediaPacket && encoding.ssrc == ssrc)
			{
				auto* rtpStream = CreateRtpStream(packet, *mediaCodec, i);

				return rtpStream;
			}
			else if (isRtxPacket && encoding.hasRtx && encoding.rtx.ssrc == ssrc)
			{
				auto it = this->mapSsrcRtpStream.find(encoding.ssrc);

				// Ignore if no stream has been created yet for the corresponding encoding.
				if (it == this->mapSsrcRtpStream.end())
				{
					MS_DEBUG_2TAGS(rtp, rtx, "ignoring RTX packet for not yet created RtpStream (ssrc lookup)");

					return nullptr;
				}

				auto* rtpStream = it->second;

				// Ensure no RTX ssrc was previously detected.
				if (rtpStream->HasRtx())
				{
					MS_DEBUG_2TAGS(rtp, rtx, "ignoring RTX packet with new ssrc (ssrc lookup)");

					return nullptr;
				}

				// Update the stream RTX data.
				rtpStream->SetRtx(payloadType, ssrc);

				// Insert the new RTX ssrc into the map.
				this->mapRtxSsrcRtpStream[ssrc] = rtpStream;

				return rtpStream;
			}
		}

		// If not found, look for an encoding matching the packet RID value.
		std::string rid;

		if (packet->ReadRid(rid))
		{
			for (size_t i{ 0 }; i < this->rtpParameters.encodings.size(); ++i)
			{
				auto& encoding = this->rtpParameters.encodings[i];

				if (encoding.rid != rid)
				{
					continue;
				}

				const auto* mediaCodec   = this->rtpParameters.GetCodecForEncoding(encoding);
				const auto* rtxCodec     = this->rtpParameters.GetRtxCodecForEncoding(encoding);
				const bool isMediaPacket = (mediaCodec->payloadType == payloadType);
				const bool isRtxPacket   = (rtxCodec && rtxCodec->payloadType == payloadType);

				if (isMediaPacket)
				{
					// Ensure no other stream already exists with same RID.
					for (auto& kv : this->mapSsrcRtpStream)
					{
						auto* rtpStream = kv.second;

						if (rtpStream->GetRid() == rid)
						{
							MS_WARN_TAG(
							  rtp, "ignoring packet with unknown ssrc but already handled RID (RID lookup)");

							return nullptr;
						}
					}

					auto* rtpStream = CreateRtpStream(packet, *mediaCodec, i);

					return rtpStream;
				}
				else if (isRtxPacket)
				{
					// Ensure a stream already exists with same RID.
					for (auto& kv : this->mapSsrcRtpStream)
					{
						auto* rtpStream = kv.second;

						if (rtpStream->GetRid() == rid)
						{
							// Ensure no RTX ssrc was previously detected.
							if (rtpStream->HasRtx())
							{
								MS_DEBUG_2TAGS(rtp, rtx, "ignoring RTX packet with new SSRC (RID lookup)");

								return nullptr;
							}

							// Update the stream RTX data.
							rtpStream->SetRtx(payloadType, ssrc);

							// Insert the new RTX ssrc into the map.
							this->mapRtxSsrcRtpStream[ssrc] = rtpStream;

							return rtpStream;
						}
					}

					MS_DEBUG_2TAGS(rtp, rtx, "ignoring RTX packet for not yet created RtpStream (RID lookup)");

					return nullptr;
				}
			}

			MS_WARN_TAG(info, "ignoring packet with unknown RID (RID lookup)");

			return nullptr;
		}

		// If not found, and there is a single encoding without ssrc and RID, this
		// may be the media or RTX stream.
		//
		// clang-format off
		if (
			this->rtpParameters.encodings.size() == 1 &&
			!this->rtpParameters.encodings[0].ssrc &&
			this->rtpParameters.encodings[0].rid.empty()
		)
		// clang-format on
		{
			auto& encoding           = this->rtpParameters.encodings[0];
			const auto* mediaCodec   = this->rtpParameters.GetCodecForEncoding(encoding);
			const auto* rtxCodec     = this->rtpParameters.GetRtxCodecForEncoding(encoding);
			const bool isMediaPacket = (mediaCodec->payloadType == payloadType);
			const bool isRtxPacket   = (rtxCodec && rtxCodec->payloadType == payloadType);

			if (isMediaPacket)
			{
				// Ensure there is no other RTP stream already.
				if (!this->mapSsrcRtpStream.empty())
				{
					MS_WARN_TAG(
					  rtp,
					  "ignoring packet with unknown ssrc not matching the already existing stream (single RtpStream lookup)");

					return nullptr;
				}

				auto* rtpStream = CreateRtpStream(packet, *mediaCodec, 0);

				return rtpStream;
			}
			else if (isRtxPacket)
			{
				// There must be already a media RTP stream.
				auto it = this->mapSsrcRtpStream.begin();

				if (it == this->mapSsrcRtpStream.end())
				{
					MS_DEBUG_2TAGS(
					  rtp, rtx, "ignoring RTX packet for not yet created RtpStream (single stream lookup)");

					return nullptr;
				}

				auto* rtpStream = it->second;

				// Ensure no RTX SSRC was previously detected.
				if (rtpStream->HasRtx())
				{
					MS_DEBUG_2TAGS(rtp, rtx, "ignoring RTX packet with new SSRC (single stream lookup)");

					return nullptr;
				}

				// Update the stream RTX data.
				rtpStream->SetRtx(payloadType, ssrc);

				// Insert the new RTX SSRC into the map.
				this->mapRtxSsrcRtpStream[ssrc] = rtpStream;

				return rtpStream;
			}
		}

		return nullptr;
	}

	RTC::RtpStreamRecv* Producer::CreateRtpStream(
	  RTC::RtpPacket* packet, const RTC::RtpCodecParameters& mediaCodec, size_t encodingIdx)
	{
		MS_TRACE();

		const uint32_t ssrc = packet->GetSsrc();

		MS_ASSERT(
		  this->mapSsrcRtpStream.find(ssrc) == this->mapSsrcRtpStream.end(),
		  "RtpStream with given SSRC already exists");
		MS_ASSERT(
		  !this->rtpStreamByEncodingIdx[encodingIdx],
		  "RtpStream for given encoding index already exists");

		auto& encoding        = this->rtpParameters.encodings[encodingIdx];
		auto& encodingMapping = this->rtpMapping.encodings[encodingIdx];

		MS_DEBUG_TAG(
		  rtp,
		  "[encodingIdx:%zu, ssrc:%" PRIu32 ", rid:%s, payloadType:%" PRIu8 "]",
		  encodingIdx,
		  ssrc,
		  encoding.rid.c_str(),
		  mediaCodec.payloadType);

		// Set stream params.
		RTC::RtpStream::Params params;

		params.encodingIdx    = encodingIdx;
		params.ssrc           = ssrc;
		params.payloadType    = mediaCodec.payloadType;
		params.mimeType       = mediaCodec.mimeType;
		params.clockRate      = mediaCodec.clockRate;
		params.rid            = encoding.rid;
		params.cname          = this->rtpParameters.rtcp.cname;
		params.spatialLayers  = encoding.spatialLayers;
		params.temporalLayers = encoding.temporalLayers;

		// Check in band FEC in codec parameters.
		if (mediaCodec.parameters.HasInteger("useinbandfec") && mediaCodec.parameters.GetInteger("useinbandfec") == 1)
		{
			MS_DEBUG_TAG(rtcp, "in band FEC enabled");

			params.useInBandFec = true;
		}

		// Check DTX in codec parameters.
		if (mediaCodec.parameters.HasInteger("usedtx") && mediaCodec.parameters.GetInteger("usedtx") == 1)
		{
			MS_DEBUG_TAG(rtp, "DTX enabled");

			params.useDtx = true;
		}

		// Check DTX in the encoding.
		if (encoding.dtx)
		{
			MS_DEBUG_TAG(rtp, "DTX enabled");

			params.useDtx = true;
		}

		for (const auto& fb : mediaCodec.rtcpFeedback)
		{
			if (!params.useNack && fb.type == "nack" && fb.parameter.empty())
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "NACK supported");

				params.useNack = true;
			}
			else if (!params.usePli && fb.type == "nack" && fb.parameter == "pli")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "PLI supported");

				params.usePli = true;
			}
			else if (!params.useFir && fb.type == "ccm" && fb.parameter == "fir")
			{
				MS_DEBUG_2TAGS(rtp, rtcp, "FIR supported");

				params.useFir = true;
			}
		}

		// Only perform RTP inactivity check on simulcast and only if there are
		// more than 1 stream.
		auto useRtpInactivityCheck =
		  this->type == RtpParameters::Type::SIMULCAST && this->rtpMapping.encodings.size() > 1;

		// Create a RtpStreamRecv for receiving a media stream.
		auto* rtpStream = new RTC::RtpStreamRecv(this, params, SendNackDelay, useRtpInactivityCheck);

		// Insert into the maps.
		this->mapSsrcRtpStream[ssrc]              = rtpStream;
		this->rtpStreamByEncodingIdx[encodingIdx] = rtpStream;
		this->rtpStreamScores[encodingIdx]        = rtpStream->GetScore();

		// A stream rebuild invalidates receipt evidence from the old generation;
		// flush the final state of the old generation first.
		MaybeLogKeyFrameSummary(ssrc, DepLibUV::GetTimeMs(), /*force=*/true);
		ClearKeyFrameCandidate(ssrc, "stream_rebuilt", /*requestRecovery=*/false);
		ResetKeyFrameGenerationState(ssrc);

		// Start the key frame cadence baseline for the new stream.
		MarkKeyFrameCadenceBaseline(ssrc, DepLibUV::GetTimeMs());

		// Set the mapped SSRC.
		this->mapRtpStreamMappedSsrc[rtpStream]             = encodingMapping.mappedSsrc;
		this->mapMappedSsrcSsrc[encodingMapping.mappedSsrc] = ssrc;

		// If the Producer is paused tell it to the new RtpStreamRecv.
		if (this->paused)
		{
			rtpStream->Pause();
		}

		// Emit the first score event right now.
		EmitScore();

		return rtpStream;
	}

	void Producer::NotifyNewRtpStream(RTC::RtpStreamRecv* rtpStream)
	{
		MS_TRACE();

		auto mappedSsrc = this->mapRtpStreamMappedSsrc.at(rtpStream);

		// Notify the listener.
		this->listener->OnProducerNewRtpStream(this, rtpStream, mappedSsrc);
	}

	inline void Producer::PreProcessRtpPacket(RTC::RtpPacket* packet)
	{
		MS_TRACE();

		if (this->kind == RTC::Media::Kind::VIDEO)
		{
			// NOTE: Remove this once framemarking draft becomes RFC.
			packet->SetFrameMarking07ExtensionId(this->rtpHeaderExtensionIds.frameMarking07);
			packet->SetFrameMarkingExtensionId(this->rtpHeaderExtensionIds.frameMarking);
		}
	}

	inline bool Producer::MangleRtpPacket(RTC::RtpPacket* packet, RTC::RtpStreamRecv* rtpStream) const
	{
		MS_TRACE();
		const uint8_t originalPayloadType = packet->GetPayloadType();
		const uint32_t originalSsrc       = packet->GetSsrc();

		// Mangle the payload type.
		{
			const uint8_t payloadType = packet->GetPayloadType();
			auto it                   = this->rtpMapping.codecs.find(payloadType);

			if (it == this->rtpMapping.codecs.end())
			{
				MS_WARN_TAG(info, "unknown payload type [payloadType:%" PRIu8 "]", payloadType);

				return false;
			}

			const uint8_t mappedPayloadType = it->second;

			packet->SetPayloadType(mappedPayloadType);
		}

		// Mangle the SSRC.
		{
			const uint32_t mappedSsrc = this->mapRtpStreamMappedSsrc.at(rtpStream);

			packet->SetSsrc(mappedSsrc);
		}

		// Mangle RTP header extensions.
		{
			thread_local static uint8_t buffer[4096];
			thread_local static std::vector<RTC::RtpPacket::GenericExtension> extensions;

			// This happens just once.
			if (extensions.capacity() != 24)
			{
				extensions.reserve(24);
			}

			extensions.clear();

			uint8_t* extenValue;
			uint8_t extenLen;
			uint8_t* bufferPtr{ buffer };

			// Add urn:ietf:params:rtp-hdrext:sdes:mid.
			{
				extenLen = RTC::MidMaxLength;

				extensions.emplace_back(
				  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID), extenLen, bufferPtr);

				bufferPtr += extenLen;
			}

			// Proxy http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time.
			extenValue = packet->GetExtension(this->rtpHeaderExtensionIds.absCaptureTime, extenLen);

			if (extenValue)
			{
				// The 16-byte form carries a capture-clock offset in addition to the
				// absolute capture timestamp. When we have an RTCP-derived clock
				// sample, rewrite the offset to the SFU clock domain. When we don't
				// yet have a sample, fall back to the 8-byte form so receivers can
				// still compute a basic end-to-end latency from the capture NTP time.
				if (extenLen == 16u)
				{
					const auto senderToLocalOffsetMs = rtpStream->GetSenderToLocalClockOffsetMs();

					if (senderToLocalOffsetMs.has_value() &&
						packet->UpdateAbsCaptureTimeOffsetMs(*senderToLocalOffsetMs))
					{
						extenLen = 16u;
					}
					else
					{
						extenLen = 8u;
					}
				}

				if (extenLen >= 8u)
				{
					std::memcpy(bufferPtr, extenValue, extenLen);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_CAPTURE_TIME),
					extenLen,
					  bufferPtr);

					bufferPtr += extenLen;
				}
			}

			if (this->kind == RTC::Media::Kind::AUDIO)
			{
				// Proxy urn:ietf:params:rtp-hdrext:ssrc-audio-level.
				extenValue = packet->GetExtension(this->rtpHeaderExtensionIds.ssrcAudioLevel, extenLen);

				if (extenValue)
				{
					std::memcpy(bufferPtr, extenValue, extenLen);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::SSRC_AUDIO_LEVEL),
					  extenLen,
					  bufferPtr);

					// Not needed since this is the latest added extension.
					// bufferPtr += extenLen;
				}
			}
			else if (this->kind == RTC::Media::Kind::VIDEO)
			{
				// Add http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time.
				// NOTE: This is for REMB.
				{
					extenLen = 3u;

					// NOTE: Add value 0. The sending Transport will update it.
					const uint32_t absSendTime{ 0u };

					Utils::Byte::Set3Bytes(bufferPtr, 0, absSendTime);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME), extenLen, bufferPtr);

					bufferPtr += extenLen;
				}

				// Add http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01.
				// NOTE: We don't include it in outbound audio packets for now.
				{
					extenLen = 2u;

					// NOTE: Add value 0. The sending Transport will update it.
					const uint16_t wideSeqNumber{ 0u };

					Utils::Byte::Set2Bytes(bufferPtr, 0, wideSeqNumber);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01),
					  extenLen,
					  bufferPtr);

					bufferPtr += extenLen;
				}

				// NOTE: Remove this once framemarking draft becomes RFC.
				// Proxy http://tools.ietf.org/html/draft-ietf-avtext-framemarking-07.
				extenValue = packet->GetExtension(this->rtpHeaderExtensionIds.frameMarking07, extenLen);

				if (extenValue)
				{
					std::memcpy(bufferPtr, extenValue, extenLen);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::FRAME_MARKING_07),
					  extenLen,
					  bufferPtr);

					bufferPtr += extenLen;
				}

				// Proxy urn:ietf:params:rtp-hdrext:framemarking.
				extenValue = packet->GetExtension(this->rtpHeaderExtensionIds.frameMarking, extenLen);

				if (extenValue)
				{
					std::memcpy(bufferPtr, extenValue, extenLen);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::FRAME_MARKING), extenLen, bufferPtr);

					bufferPtr += extenLen;
				}

				// Proxy urn:3gpp:video-orientation.
				extenValue = packet->GetExtension(this->rtpHeaderExtensionIds.videoOrientation, extenLen);

				if (extenValue)
				{
					std::memcpy(bufferPtr, extenValue, extenLen);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::VIDEO_ORIENTATION),
					  extenLen,
					  bufferPtr);

					bufferPtr += extenLen;
				}

				// Proxy urn:ietf:params:rtp-hdrext:toffset.
				extenValue = packet->GetExtension(this->rtpHeaderExtensionIds.toffset, extenLen);

				if (extenValue)
				{
					std::memcpy(bufferPtr, extenValue, extenLen);

					extensions.emplace_back(
					  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TOFFSET), extenLen, bufferPtr);

					// Not needed since this is the latest added extension.
					// bufferPtr += extenLen;
				}
			}

			// Set the new extensions into the packet using One-Byte format.
			if (!packet->SetExtensions(1, extensions))
			{
				packet->SetPayloadType(originalPayloadType);
				packet->SetSsrc(originalSsrc);
				MS_WARN_TAG(info, "RTP packet has insufficient capacity for header extensions");
				return false;
			}

			// Assign mediasoup RTP header extension ids (just those that mediasoup may
			// be interested in after passing it to the Router).
			packet->SetMidExtensionId(static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::MID));
			packet->SetAbsSendTimeExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_SEND_TIME));
			packet->SetAbsCaptureTimeExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::ABS_CAPTURE_TIME));
			packet->SetTransportWideCc01ExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::TRANSPORT_WIDE_CC_01));
			// NOTE: Remove this once framemarking draft becomes RFC.
			packet->SetFrameMarking07ExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::FRAME_MARKING_07));
			packet->SetFrameMarkingExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::FRAME_MARKING));
			packet->SetSsrcAudioLevelExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::SSRC_AUDIO_LEVEL));
			packet->SetVideoOrientationExtensionId(
			  static_cast<uint8_t>(RTC::RtpHeaderExtensionUri::Type::VIDEO_ORIENTATION));
		}

		return true;
	}

	inline void Producer::PostProcessRtpPacket(RTC::RtpPacket* packet)
	{
		MS_TRACE();

		if (this->kind == RTC::Media::Kind::VIDEO)
		{
			bool camera;
			bool flip;
			uint16_t rotation;

			if (packet->ReadVideoOrientation(camera, flip, rotation))
			{
				// If video orientation was not yet detected or any value has changed,
				// emit event.
				// clang-format off
				if (
					!this->videoOrientationDetected ||
					camera != this->videoOrientation.camera ||
					flip != this->videoOrientation.flip ||
					rotation != this->videoOrientation.rotation
				)
				// clang-format on
				{
					this->videoOrientationDetected  = true;
					this->videoOrientation.camera   = camera;
					this->videoOrientation.flip     = flip;
					this->videoOrientation.rotation = rotation;

					auto notification = FBS::Producer::CreateVideoOrientationChangeNotification(
					  this->shared->channelNotifier->GetBufferBuilder(),
					  this->videoOrientation.camera,
					  this->videoOrientation.flip,
					  this->videoOrientation.rotation);

					this->shared->channelNotifier->Emit(
					  this->id,
					  FBS::Notification::Event::PRODUCER_VIDEO_ORIENTATION_CHANGE,
					  FBS::Notification::Body::Producer_VideoOrientationChangeNotification,
					  notification);
				}
			}
		}
	}

	inline void Producer::EmitScore() const
	{
		MS_TRACE();

		std::vector<flatbuffers::Offset<FBS::Producer::Score>> scores;

		for (const auto* rtpStream : this->rtpStreamByEncodingIdx)
		{
			if (!rtpStream)
			{
				continue;
			}

			scores.emplace_back(FBS::Producer::CreateScoreDirect(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  rtpStream->GetEncodingIdx(),
			  rtpStream->GetSsrc(),
			  !rtpStream->GetRid().empty() ? rtpStream->GetRid().c_str() : nullptr,
			  rtpStream->GetScore(),
			  rtpStream->GetInstantScore()));
		}

		auto notification = FBS::Producer::CreateScoreNotificationDirect(
		  this->shared->channelNotifier->GetBufferBuilder(), &scores);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::PRODUCER_SCORE,
		  FBS::Notification::Body::Producer_ScoreNotification,
		  notification);
	}

	inline void Producer::EmitRtpActivityTransition(
	  RTC::RtpStreamRecv* rtpStream,
	  bool rtpActive,
	  uint64_t transitionAtMs,
	  uint64_t workerEventAtMs,
	  uint64_t lastRtpActivityAtMs,
	  uint32_t rtpActivityThresholdMs,
	  uint64_t rtpActivityStateVersion) const
	{
		MS_TRACE();

		if (!rtpStream)
		{
			return;
		}

		auto& builder = this->shared->channelNotifier->GetBufferBuilder();
		const auto rtxSsrc = rtpStream->HasRtx()
		                       ? flatbuffers::Optional<uint32_t>(rtpStream->GetRtxSsrc())
		                       : flatbuffers::nullopt;
		auto notification = FBS::Producer::CreateRtpActivityTransitionNotificationDirect(
		  builder,
		  rtpStream->GetEncodingIdx(),
		  rtpStream->GetSsrc(),
		  !rtpStream->GetRid().empty() ? rtpStream->GetRid().c_str() : nullptr,
		  rtxSsrc,
		  rtpActive,
		  rtpActivityStateVersion,
		  transitionAtMs,
		  workerEventAtMs,
		  lastRtpActivityAtMs,
		  rtpActivityThresholdMs);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::PRODUCER_RTP_ACTIVITY_TRANSITION,
		  FBS::Notification::Body::Producer_RtpActivityTransitionNotification,
		  notification);
	}

	inline void Producer::EmitTraceEventRtpAndKeyFrameTypes(RTC::RtpPacket* packet, bool isRtx) const
	{
		MS_TRACE();

		if (this->traceEventTypes.keyframe && packet->IsKeyFrame())
		{
			auto rtpPacketDump = packet->FillBuffer(this->shared->channelNotifier->GetBufferBuilder());
			auto traceInfo     = FBS::Producer::CreateKeyFrameTraceInfo(
        this->shared->channelNotifier->GetBufferBuilder(), rtpPacketDump, isRtx);

			auto notification = FBS::Producer::CreateTraceNotification(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  FBS::Producer::TraceEventType::KEYFRAME,
			  DepLibUV::GetTimeMs(),
			  FBS::Common::TraceDirection::DIRECTION_IN,
			  FBS::Producer::TraceInfo::KeyFrameTraceInfo,
			  traceInfo.Union());

			EmitTraceEvent(notification);
		}
		else if (this->traceEventTypes.rtp)
		{
			auto rtpPacketDump = packet->FillBuffer(this->shared->channelNotifier->GetBufferBuilder());
			auto traceInfo     = FBS::Producer::CreateRtpTraceInfo(
        this->shared->channelNotifier->GetBufferBuilder(), rtpPacketDump, isRtx);

			auto notification = FBS::Producer::CreateTraceNotification(
			  this->shared->channelNotifier->GetBufferBuilder(),
			  FBS::Producer::TraceEventType::RTP,
			  DepLibUV::GetTimeMs(),
			  FBS::Common::TraceDirection::DIRECTION_IN,
			  FBS::Producer::TraceInfo::RtpTraceInfo,
			  traceInfo.Union());

			EmitTraceEvent(notification);
		}
	}

	inline void Producer::EmitTraceEventPliType(uint32_t ssrc) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.pli)
		{
			return;
		}

		auto traceInfo =
		  FBS::Producer::CreatePliTraceInfo(this->shared->channelNotifier->GetBufferBuilder(), ssrc);

		auto notification = FBS::Producer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Producer::TraceEventType::PLI,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_OUT,
		  FBS::Producer::TraceInfo::PliTraceInfo,
		  traceInfo.Union());

		EmitTraceEvent(notification);
	}

	inline void Producer::EmitTraceEventFirType(uint32_t ssrc) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.fir)
		{
			return;
		}

		auto traceInfo =
		  FBS::Producer::CreateFirTraceInfo(this->shared->channelNotifier->GetBufferBuilder(), ssrc);

		auto notification = FBS::Producer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Producer::TraceEventType::FIR,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_OUT,
		  FBS::Producer::TraceInfo::FirTraceInfo,
		  traceInfo.Union());

		EmitTraceEvent(notification);
	}

	inline void Producer::EmitTraceEventNackType() const
	{
		MS_TRACE();

		if (!this->traceEventTypes.nack)
		{
			return;
		}

		auto notification = FBS::Producer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Producer::TraceEventType::NACK,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_OUT);

		EmitTraceEvent(notification);
	}

	inline void Producer::EmitTraceEventSrType(RTC::RTCP::SenderReport* report) const
	{
		MS_TRACE();

		if (!this->traceEventTypes.sr)
		{
			return;
		}

		auto traceInfo = FBS::Producer::CreateSrTraceInfo(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  report->GetSsrc(),
		  report->GetNtpSec(),
		  report->GetNtpFrac(),
		  report->GetRtpTs(),
		  report->GetPacketCount(),
		  report->GetOctetCount());

		auto notification = FBS::Producer::CreateTraceNotification(
		  this->shared->channelNotifier->GetBufferBuilder(),
		  FBS::Producer::TraceEventType::SR,
		  DepLibUV::GetTimeMs(),
		  FBS::Common::TraceDirection::DIRECTION_IN,
		  FBS::Producer::TraceInfo::SrTraceInfo,
		  traceInfo.Union());

		EmitTraceEvent(notification);
	}

	inline void Producer::EmitTraceEvent(
	  flatbuffers::Offset<FBS::Producer::TraceNotification>& notification) const
	{
		MS_TRACE();

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::PRODUCER_TRACE,
		  FBS::Notification::Body::Producer_TraceNotification,
		  notification);
	}

	inline void Producer::OnRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore)
	{
		MS_TRACE();

		// Update the vector of scores.
		this->rtpStreamScores[rtpStream->GetEncodingIdx()] = score;

		// Notify the listener.
		this->listener->OnProducerRtpStreamScore(
		  this, static_cast<RTC::RtpStreamRecv*>(rtpStream), score, previousScore);

		// Emit the score event.
		EmitScore();
	}

	inline void Producer::OnRtpStreamSendRtcpPacket(
	  RTC::RtpStreamRecv* /*rtpStream*/, RTC::RTCP::Packet* packet)
	{
		switch (packet->GetType())
		{
			case RTC::RTCP::Type::PSFB:
			{
				auto* feedback = static_cast<RTC::RTCP::FeedbackPsPacket*>(packet);

				switch (feedback->GetMessageType())
				{
					case RTC::RTCP::FeedbackPs::MessageType::PLI:
					{
						// May emit 'trace' event.
						EmitTraceEventPliType(feedback->GetMediaSsrc());

						break;
					}

					case RTC::RTCP::FeedbackPs::MessageType::FIR:
					{
						// May emit 'trace' event.
						EmitTraceEventFirType(feedback->GetMediaSsrc());

						break;
					}

					default:;
				}

				break;
			}

			case RTC::RTCP::Type::RTPFB:
			{
				auto* feedback = static_cast<RTC::RTCP::FeedbackRtpPacket*>(packet);

				switch (feedback->GetMessageType())
				{
					case RTC::RTCP::FeedbackRtp::MessageType::NACK:
					{
						// May emit 'trace' event.
						EmitTraceEventNackType();

						break;
					}

					default:;
				}

				break;
			}

			default:;
		}

		// Notify the listener.
		this->listener->OnProducerSendRtcpPacket(this, packet);
	}

	inline void Producer::OnRtpStreamNeedWorstRemoteFractionLost(
	  RTC::RtpStreamRecv* rtpStream, uint8_t& worstRemoteFractionLost)
	{
		auto mappedSsrc = this->mapRtpStreamMappedSsrc.at(rtpStream);

		// Notify the listener.
		this->listener->OnProducerNeedWorstRemoteFractionLost(this, mappedSsrc, worstRemoteFractionLost);
	}

	inline void Producer::OnRtpStreamRtpActivityTransition(
	  RTC::RtpStreamRecv* rtpStream,
	  bool rtpActive,
	  uint64_t transitionAtMs,
	  uint64_t workerEventAtMs,
	  uint64_t lastRtpActivityAtMs,
	  uint32_t rtpActivityThresholdMs,
	  uint64_t rtpActivityStateVersion)
	{
		EmitRtpActivityTransition(
		  rtpStream,
		  rtpActive,
		  transitionAtMs,
		  workerEventAtMs,
		  lastRtpActivityAtMs,
		  rtpActivityThresholdMs,
		  rtpActivityStateVersion);
	}

	void Producer::OnTimer(TimerHandle* timer)
	{
		MS_TRACE();

		if (timer == this->keyFrameEvidenceTimer)
		{
			FlushDirtyKeyFrameSummaries(DepLibUV::GetTimeMs());
			return;
		}

		for (auto& ssrcKv : this->mapSsrcKeyFrameCandidates)
		{
			for (auto& timestampKv : ssrcKv.second)
			{
				if (timestampKv.second->timer != timer)
				{
					continue;
				}

				FinalizeKeyFrameCandidate(
				  ssrcKv.first,
				  timestampKv.second,
				  "timeout",
				  /*complete=*/false,
				  /*requestRecovery=*/true,
				  DepLibUV::GetTimeMs());
				return;
			}
		}
	}

	void Producer::OnRtpStreamKeyFrameRequired(RTC::RtpStreamRecv* rtpStream)
	{
		MS_TRACE();

		// NACK-generator overflow means sustained unrecoverable uplink loss.
		// In cadence mode the request must go through the manager so pending
		// dedup, the coalescing window and the request-spacing bookkeeping
		// apply; an already in-flight request then covers this need instead of
		// producing another uncoordinated RTCP feedback. This also holds while
		// the producer is paused: the request serves receive-stream recovery
		// (upstream behavior always sent it), and the manager coordinates it;
		// the consumer-facing key frame on resume is handled separately by the
		// forced request in the resume path.
		// Legacy mode (delay 0, or a video producer without a manager) keeps
		// the previous behavior: forward the key frame request directly.
		if (this->keyFrameRequestDelay > 0u && this->keyFrameRequestManager)
		{
			this->keyFrameRequestManager->KeyFrameNeeded(rtpStream->GetSsrc());

			return;
		}

		rtpStream->RequestKeyFrame();
	}

	inline void Producer::OnKeyFrameNeeded(
	  RTC::KeyFrameRequestManager* /*keyFrameRequestManager*/, uint32_t ssrc)
	{
		MS_TRACE();

		auto it = this->mapSsrcRtpStream.find(ssrc);

		if (it == this->mapSsrcRtpStream.end())
		{
			MS_WARN_2TAGS(rtcp, rtx, "no associated RtpStream found [ssrc:%" PRIu32 "]", ssrc);

			return;
		}

		auto* rtpStream = it->second;

		// Every forwarded request (new stream, resume, coalesced internal
		// request, watchdog fire) passes here: record it so the cadence
		// watchdog enforces a minimum spacing against any prior request, not
		// just against its own fires.
		MarkKeyFrameRequestSent(ssrc, DepLibUV::GetTimeMs());

		rtpStream->RequestKeyFrame();
	}
} // namespace RTC

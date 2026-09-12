#define MS_CLASS "RTC::PipeConsumer"
#define MS_LOG_DEV_LEVEL 2

#include "RTC/PipeConsumer.hpp"
#include "RTC/ProbeEgressAdapter.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "RTC/Codecs/Tools.hpp"

namespace RTC
{
	/* Instance methods. */

	PipeConsumer::PipeConsumer(
	  RTC::Shared* shared,
	  const std::string& id,
	  const std::string& producerId,
	  RTC::Consumer::Listener* listener,
	  const FBS::Transport::ConsumeRequest* data)
	  : RTC::Consumer::Consumer(shared, id, producerId, listener, data, RTC::RtpParameters::Type::PIPE)
	{
		MS_TRACE();

		// Ensure there are as many encodings as consumable encodings.
		if (this->rtpParameters.encodings.size() != this->consumableRtpEncodings.size())
		{
			MS_THROW_TYPE_ERROR("number of rtpParameters.encodings and consumableRtpEncodings do not match");
		}

		auto& encoding         = this->rtpParameters.encodings[0];
		const auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		this->keyFrameSupported = RTC::Codecs::Tools::CanBeKeyFrame(mediaCodec->mimeType);

		// Create RtpStreamSend instances.
		CreateRtpStreams();

		// NOTE: This may throw.
		this->shared->channelMessageRegistrator->RegisterHandler(
		  this->id,
		  /*channelRequestHandler*/ this,
		  /*channelNotificationHandler*/ nullptr);
	}

	PipeConsumer::~PipeConsumer()
	{
		MS_TRACE();

		this->shared->channelMessageRegistrator->UnregisterHandler(this->id);

		for (auto* rtpStream : this->rtpStreams)
		{
			delete rtpStream;
		}
		this->rtpStreams.clear();
		this->mapMappedSsrcSsrc.clear();
		this->mapSsrcRtpStream.clear();
	}

	flatbuffers::Offset<FBS::Consumer::DumpResponse> PipeConsumer::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Call the parent method.
		auto base = RTC::Consumer::FillBuffer(builder);

		// Add rtpStreams.
		std::vector<flatbuffers::Offset<FBS::RtpStream::Dump>> rtpStreams;
		rtpStreams.reserve(this->rtpStreams.size());

		for (const auto* rtpStream : this->rtpStreams)
		{
			rtpStreams.emplace_back(rtpStream->FillBuffer(builder));
		}

		auto dump = FBS::Consumer::CreateConsumerDumpDirect(builder, base, &rtpStreams);

		return FBS::Consumer::CreateDumpResponse(builder, dump);
	}

	flatbuffers::Offset<FBS::Consumer::GetStatsResponse> PipeConsumer::FillBufferStats(
	  flatbuffers::FlatBufferBuilder& builder)
	{
		MS_TRACE();

		std::vector<flatbuffers::Offset<FBS::RtpStream::Stats>> rtpStreams;
		rtpStreams.reserve(this->rtpStreams.size());

		// Add stats of our send streams.
		for (auto* rtpStream : this->rtpStreams)
		{
			rtpStreams.emplace_back(rtpStream->FillBufferStats(builder));
		}

		return FBS::Consumer::CreateGetStatsResponseDirect(builder, &rtpStreams);
	}

	flatbuffers::Offset<FBS::Consumer::ConsumerScore> PipeConsumer::FillBufferScore(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		MS_ASSERT(this->producerRtpStreamScores, "producerRtpStreamScores not set");

		// NOTE: Hardcoded values in PipeTransport.
		return FBS::Consumer::CreateConsumerScoreDirect(builder, 10, 10, this->producerRtpStreamScores);
	}

	void PipeConsumer::HandleRequest(Channel::ChannelRequest* request)
	{
		MS_TRACE();

		switch (request->method)
		{
			case Channel::ChannelRequest::Method::CONSUMER_DUMP:
			{
				auto dumpOffset = FillBuffer(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::Consumer_DumpResponse, dumpOffset);

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_REQUEST_KEY_FRAME:
			{
				if (IsActive())
				{
					RequestKeyFrame();
				}

				request->Accept();

				break;
			}

			case Channel::ChannelRequest::Method::CONSUMER_SET_PREFERRED_LAYERS:
			{
				// Accept with empty preferred layers object.

				auto responseOffset =
				  FBS::Consumer::CreateSetPreferredLayersResponse(request->GetBufferBuilder());

				request->Accept(FBS::Response::Body::Consumer_SetPreferredLayersResponse, responseOffset);

				break;
			}

			default:
			{
				// Pass it to the parent class.
				RTC::Consumer::HandleRequest(request);
			}
		}
	}

	void PipeConsumer::ProducerRtpStream(RTC::RtpStreamRecv* /*rtpStream*/, uint32_t /*mappedSsrc*/)
	{
		MS_TRACE();

		// Do nothing.
	}

	void PipeConsumer::ProducerNewRtpStream(RTC::RtpStreamRecv* /*rtpStream*/, uint32_t /*mappedSsrc*/)
	{
		MS_TRACE();

		MS_WARN_DEV(
		  "producer signaled a new RTP stream for PipeConsumer; next packets may require re-sync [consumerId:%s]",
		  this->id.c_str());
	}

	void PipeConsumer::ProducerRtpStreamScore(
	  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/)
	{
		MS_TRACE();

		// Do nothing.
	}

	void PipeConsumer::ProducerRtcpSenderReport(RTC::RtpStreamRecv* /*rtpStream*/, bool /*first*/)
	{
		MS_TRACE();

		// Do nothing.
	}

	uint8_t PipeConsumer::GetBitratePriority() const
	{
		MS_TRACE();

		// PipeConsumer does not play the BWE game.
		return 0u;
	}

	uint32_t PipeConsumer::IncreaseLayer(uint32_t /*bitrate*/, bool /*considerLoss*/)
	{
		MS_TRACE();

		// PipeConsumer does not play the BWE game.
		return 0u;
	}

	void PipeConsumer::ApplyLayers()
	{
		MS_TRACE();

		// PipeConsumer does not play the BWE game.
	}

	uint32_t PipeConsumer::GetDesiredBitrate() const
	{
		MS_TRACE();

		// PipeConsumer does not play the BWE game.
		return 0u;
	}

	void PipeConsumer::SendRtpPacket(
	  RTC::RtpPacket* packet, RTC::Consumer::RtpPacketFanoutContext& fanoutContext)
	{
		MS_TRACE();
		RTC::Consumer::RtpPacketMutationGuard packetGuard(packet);

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.consumerId = this->id;
#endif

		if (!IsActive())
		{
#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::CONSUMER_INACTIVE);
#endif

			return;
		}

		auto payloadType = packet->GetPayloadType();

		// NOTE: This may happen if this Consumer supports just some codecs of those
		// in the corresponding Producer.
		if (!this->supportedCodecPayloadTypes[payloadType])
		{
			MS_DEBUG_DEV("payload type not supported [payloadType:%" PRIu8 "]", payloadType);

#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::UNSUPPORTED_PAYLOAD_TYPE);
#endif

			return;
		}

		auto ssrc           = this->mapMappedSsrcSsrc.at(packet->GetSsrc());
		auto* rtpStream     = this->mapSsrcRtpStream.at(ssrc);
		auto& syncRequired  = this->mapRtpStreamSyncRequired.at(rtpStream);
		auto& rtpSeqManager = this->mapRtpStreamRtpSeqManager.at(rtpStream);

		// If we need to sync, support key frames and this is not a key frame, ignore
		// the packet.
		if (syncRequired && this->keyFrameSupported && !packet->IsKeyFrame())
		{
			MS_WARN_DEV(
			  "pipe consumer dropping packet while waiting for sync key frame [consumerId:%s, mappedSsrc:%" PRIu32
			  ", consumerSsrc:%" PRIu32 ", origSeq:%" PRIu16 ", payloadType:%" PRIu8 "]",
			  this->id.c_str(),
			  packet->GetSsrc(),
			  ssrc,
			  packet->GetSequenceNumber(),
			  payloadType);
#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::NOT_A_KEYFRAME);
#endif

			return;
		}

		// Whether this is the first packet after re-sync.
		const bool isSyncPacket = syncRequired;

		// Sync sequence number and timestamp if required.
		if (isSyncPacket)
		{
			if (packet->IsKeyFrame())
			{
				MS_DEBUG_TAG(rtp, "sync key frame received");
			}

			MS_WARN_DEV(
			  "pipe consumer seq sync [consumerId:%s, mappedSsrc:%" PRIu32 ", consumerSsrc:%" PRIu32
			  ", origSeq:%" PRIu16 ", syncBase:%" PRIu16 ", maxInputBefore:%" PRIu16
			  ", maxOutputBefore:%" PRIu16 "]",
			  this->id.c_str(),
			  packet->GetSsrc(),
			  ssrc,
			  packet->GetSequenceNumber(),
			  static_cast<uint16_t>(packet->GetSequenceNumber() - 1),
			  rtpSeqManager.GetMaxInput(),
			  rtpSeqManager.GetMaxOutput());

			rtpSeqManager.Sync(packet->GetSequenceNumber() - 1);

			syncRequired = false;
		}

		// Update RTP seq number and timestamp.
		uint16_t seq;

		rtpSeqManager.Input(packet->GetSequenceNumber(), seq);

		const int32_t sendSeqDelta = static_cast<int32_t>(
			static_cast<int16_t>(seq - packet->GetSequenceNumber()));
		if (std::abs(sendSeqDelta) > 32)
		{
			MS_WARN_DEV(
			  "pipe consumer seq remap [consumerId:%s, mappedSsrc:%" PRIu32 ", consumerSsrc:%" PRIu32
			  ", origSeq:%" PRIu16 ", sendSeq:%" PRIu16 ", delta:%" PRIi32 "]",
			  this->id.c_str(),
			  packet->GetSsrc(),
			  ssrc,
			  packet->GetSequenceNumber(),
			  seq,
			  sendSeqDelta);
		}

		// Save original packet fields.
		auto origSsrc = packet->GetSsrc();
		auto origSeq  = packet->GetSequenceNumber();

		// Rewrite packet.
		packet->SetSsrc(ssrc);
		packet->SetSequenceNumber(seq);

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.sendRtpTimestamp = packet->GetTimestamp();
		packet->logger.sendSeqNumber    = seq;
#endif

		if (isSyncPacket)
		{
			MS_DEBUG_TAG(
			  rtp,
			  "sending sync packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [ssrc:%" PRIu32 ", seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSsrc,
			  origSeq);
		}

		// PipeConsumers preserve extension layout and payload, so one lazy clone can
		// be shared within this fanout. Each retransmission item stores its own SSRC,
		// sequence number and timestamp and restores them before the send callback.
		// Keep this profile separate from Simple/other consumer packet variants.
		if (rtpStream->ReceivePacket(packet, fanoutContext.pipePacket))
		{
			// Send the packet.
			this->listener->OnConsumerSendRtpPacket(this, packet);

			// May emit 'trace' event.
			EmitTraceEventRtpAndKeyFrameTypes(packet);
		}
		else
		{
			MS_WARN_TAG(
			  rtp,
			  "failed to send packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [ssrc:%" PRIu32 ", seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSsrc,
			  origSeq);
		}

	}

	bool PipeConsumer::GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs)
	{
		MS_TRACE();

		// Special condition for PipeConsumer since this method will be called in a loop for
		// each stream in this PipeConsumer.
		// clang-format off
		if (
			nowMs != this->lastRtcpSentTime &&
			static_cast<float>((nowMs - this->lastRtcpSentTime) * 1.15) < this->maxRtcpInterval
		)
		// clang-format on
		{
			return true;
		}

		std::vector<RTCP::SenderReport*> senderReports;
		std::vector<RTCP::SdesChunk*> sdesChunks;
		std::vector<RTCP::DelaySinceLastRr::SsrcInfo*> delaySinceLastRrSsrcInfos;

		for (auto* rtpStream : this->rtpStreams)
		{
			auto* report = rtpStream->GetRtcpSenderReport(nowMs);

			if (!report)
			{
				continue;
			}

			senderReports.push_back(report);

			// Build SDES chunk for this sender.
			auto* sdesChunk = rtpStream->GetRtcpSdesChunk();
			sdesChunks.push_back(sdesChunk);

			auto* delaySinceLastRrSsrcInfo = rtpStream->GetRtcpXrDelaySinceLastRrSsrcInfo(nowMs);

			if (delaySinceLastRrSsrcInfo)
			{
				delaySinceLastRrSsrcInfos.push_back(delaySinceLastRrSsrcInfo);
			}
		}

		// RTCP Compound packet buffer cannot hold the data.
		if (!packet->Add(senderReports, sdesChunks, delaySinceLastRrSsrcInfos))
		{
			return false;
		}

		this->lastRtcpSentTime = nowMs;

		return true;
	}

	void PipeConsumer::NeedWorstRemoteFractionLost(uint32_t /*mappedSsrc*/, uint8_t& worstRemoteFractionLost)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return;
		}

		for (auto* rtpStream : this->rtpStreams)
		{
			auto fractionLost = rtpStream->GetFractionLost();

			// If our fraction lost is worse than the given one, update it.
			if (fractionLost > worstRemoteFractionLost)
			{
				worstRemoteFractionLost = fractionLost;
			}
		}
	}

	void PipeConsumer::ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return;
		}

		// May emit 'trace' event.
		EmitTraceEventNackType();

		auto ssrc       = nackPacket->GetMediaSsrc();
		auto* rtpStream = this->mapSsrcRtpStream.at(ssrc);

		rtpStream->ReceiveNack(nackPacket);
	}

	void PipeConsumer::ReceiveKeyFrameRequest(RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t ssrc)
	{
		MS_TRACE();

		switch (messageType)
		{
			case RTC::RTCP::FeedbackPs::MessageType::PLI:
			{
				EmitTraceEventPliType(ssrc);

				break;
			}

			case RTC::RTCP::FeedbackPs::MessageType::FIR:
			{
				EmitTraceEventFirType(ssrc);

				break;
			}

			default:;
		}

		auto* rtpStream = this->mapSsrcRtpStream.at(ssrc);

		rtpStream->ReceiveKeyFrameRequest(messageType);

		if (IsActive())
		{
			RequestKeyFrame(/*fromViewerRtcp=*/true);
		}
	}

	void PipeConsumer::ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();

		auto* rtpStream = this->mapSsrcRtpStream.at(report->GetSsrc());

		rtpStream->ReceiveRtcpReceiverReport(report);
	}

	void PipeConsumer::ReceiveRtcpXrReceiverReferenceTime(RTC::RTCP::ReceiverReferenceTime* report)
	{
		MS_TRACE();

		for (auto* rtpStream : this->rtpStreams)
		{
			rtpStream->ReceiveRtcpXrReceiverReferenceTime(report);
		}
	}

	uint32_t PipeConsumer::GetTransmissionRate(uint64_t nowMs)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return 0u;
		}

		uint32_t rate{ 0u };

		for (auto* rtpStream : this->rtpStreams)
		{
			rate += rtpStream->GetBitrate(nowMs);
		}

		return rate;
	}

	float PipeConsumer::GetRtt() const
	{
		MS_TRACE();

		float rtt{ 0 };

		for (auto* rtpStream : this->rtpStreams)
		{
			if (rtpStream->GetRtt() > rtt)
			{
				rtt = rtpStream->GetRtt();
			}
		}

		return rtt;
	}

	void PipeConsumer::UserOnTransportConnected()
	{
		MS_TRACE();

		for (auto& kv : this->mapRtpStreamSyncRequired)
		{
			MS_WARN_DEV(
			  "marking pipe consumer stream sync-required on transport connected [consumerId:%s]",
			  this->id.c_str());
			kv.second = true;
		}

		if (IsActive())
		{
			for (auto* rtpStream : this->rtpStreams)
			{
				rtpStream->Resume();
			}

			RequestKeyFrame();
		}
	}

	void PipeConsumer::UserOnTransportDisconnected()
	{
		MS_TRACE();

		for (auto* rtpStream : this->rtpStreams)
		{
			rtpStream->Pause();
		}
	}

	void PipeConsumer::UserOnPaused()
	{
		MS_TRACE();

		for (auto* rtpStream : this->rtpStreams)
		{
			rtpStream->Pause();
		}
	}

	void PipeConsumer::UserOnResumed()
	{
		MS_TRACE();

		for (auto& kv : this->mapRtpStreamSyncRequired)
		{
			MS_WARN_DEV(
			  "marking pipe consumer stream sync-required on consumer resumed [consumerId:%s]",
			  this->id.c_str());
			kv.second = true;
		}

		if (IsActive())
		{
			for (auto* rtpStream : this->rtpStreams)
			{
				rtpStream->Resume();
			}

			RequestKeyFrame();
		}
	}

	void PipeConsumer::CreateRtpStreams()
	{
		MS_TRACE();

		// NOTE: Here we know that SSRCs in Consumer's rtpParameters must be the same
		// as in the given consumableRtpEncodings.
		for (size_t idx{ 0u }; idx < this->rtpParameters.encodings.size(); ++idx)
		{
			auto& encoding           = this->rtpParameters.encodings[idx];
			const auto* mediaCodec   = this->rtpParameters.GetCodecForEncoding(encoding);
			auto& consumableEncoding = this->consumableRtpEncodings[idx];

			MS_DEBUG_TAG(
			  rtp, "[ssrc:%" PRIu32 ", payloadType:%" PRIu8 "]", encoding.ssrc, mediaCodec->payloadType);

			// Set stream params.
			RTC::RtpStream::Params params;

			params.encodingIdx    = idx;
			params.ssrc           = encoding.ssrc;
			params.payloadType    = mediaCodec->payloadType;
			params.mimeType       = mediaCodec->mimeType;
			params.clockRate      = mediaCodec->clockRate;
			params.cname          = this->rtpParameters.rtcp.cname;
			params.spatialLayers  = encoding.spatialLayers;
			params.temporalLayers = encoding.temporalLayers;

			// Check in band FEC in codec parameters.
			if (mediaCodec->parameters.HasInteger("useinbandfec") && mediaCodec->parameters.GetInteger("useinbandfec") == 1)
			{
				MS_DEBUG_TAG(rtp, "in band FEC enabled");

				params.useInBandFec = true;
			}

			// Check DTX in codec parameters.
			if (mediaCodec->parameters.HasInteger("usedtx") && mediaCodec->parameters.GetInteger("usedtx") == 1)
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

			for (const auto& fb : mediaCodec->rtcpFeedback)
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

			auto* rtpStream = new RTC::RtpStreamSend(this, params, this->rtpParameters.mid);

			// If the Consumer is paused, tell the RtpStreamSend.
			if (IsPaused() || IsProducerPaused())
			{
				rtpStream->Pause();
			}

			const auto* rtxCodec = this->rtpParameters.GetRtxCodecForEncoding(encoding);

			if (rtxCodec && encoding.hasRtx)
			{
				rtpStream->SetRtx(rtxCodec->payloadType, encoding.rtx.ssrc);
			}

			this->rtpStreams.push_back(rtpStream);
			this->mapMappedSsrcSsrc[consumableEncoding.ssrc] = encoding.ssrc;
			this->mapSsrcRtpStream[encoding.ssrc]            = rtpStream;
			this->mapRtpStreamSyncRequired[rtpStream]        = false;
			this->mapRtpStreamRtpSeqManager[rtpStream];

			if (this->shared && this->shared->probeEgressAdapter)
			{
				RTC::ProbeEgressAdapter::StreamMetadata metadata;
				metadata.consumerId   = this->id;
				metadata.producerId   = this->producerId;
				metadata.codecMime    = mediaCodec->mimeType.ToString();
				metadata.mediaKind    = this->kind;
				metadata.consumerSsrc = encoding.ssrc;
				metadata.producerSsrc = consumableEncoding.ssrc;
				metadata.payloadType  = mediaCodec->payloadType;
				metadata.clockRate    = mediaCodec->clockRate;

				this->shared->probeEgressAdapter->RegisterConsumerSsrc(metadata);

				if (rtxCodec && encoding.hasRtx)
				{
					RTC::ProbeEgressAdapter::StreamMetadata rtxMetadata = metadata;
					rtxMetadata.codecMime = rtxCodec->mimeType.ToString();
					rtxMetadata.consumerSsrc = encoding.rtx.ssrc;
					rtxMetadata.producerSsrc = consumableEncoding.rtx.ssrc;
					rtxMetadata.payloadType = rtxCodec->payloadType;
					rtxMetadata.isRtx = true;
					rtxMetadata.pairedConsumerSsrc = metadata.consumerSsrc;
					rtxMetadata.pairedProducerSsrc = metadata.producerSsrc;
					rtxMetadata.pairedPayloadType = metadata.payloadType;
					rtxMetadata.apt = metadata.payloadType;
					this->shared->probeEgressAdapter->RegisterConsumerSsrc(rtxMetadata);
				}
			}
		}
	}

	void PipeConsumer::RequestKeyFrame(bool fromViewerRtcp)
	{
		MS_TRACE();

		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return;
		}

		for (auto& consumableRtpEncoding : this->consumableRtpEncodings)
		{
			auto mappedSsrc = consumableRtpEncoding.ssrc;

			this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc, fromViewerRtcp);
		}
	}

	inline void PipeConsumer::OnRtpStreamScore(
	  RTC::RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/)
	{
		MS_TRACE();

		// Do nothing.
	}

	inline void PipeConsumer::OnRtpStreamRetransmitRtpPacket(
	  RTC::RtpStreamSend* rtpStream, RTC::RtpPacket* packet)
	{
		MS_TRACE();

		this->listener->OnConsumerRetransmitRtpPacket(this, packet);

		// May emit 'trace' event.
		EmitTraceEventRtpAndKeyFrameTypes(packet, rtpStream->HasRtx());
	}
} // namespace RTC

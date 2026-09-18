#include "FBS/consumer.h"
#define MS_CLASS "RTC::SimpleConsumer"
#define MS_LOG_DEV_LEVEL 2

#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"
#include "Utils.hpp"
#include "RTC/Codecs/Tools.hpp"
#include "RTC/SimpleConsumer.hpp"
#include <cstring>
#include <limits>
#include <new>

namespace RTC
{
	namespace
	{
		constexpr uint8_t H264NalTypeSei{ 6u };
		constexpr uint8_t H264NalTypeSps{ 7u };
		constexpr uint8_t H264NalTypePps{ 8u };
		constexpr uint8_t H264NalTypeAud{ 9u };
		constexpr uint8_t H264NalTypeStapA{ 24u };
		constexpr uint8_t H265NalTypeVps{ 32u };
		constexpr uint8_t H265NalTypeSps{ 33u };
		constexpr uint8_t H265NalTypePps{ 34u };
		constexpr uint8_t H265NalTypeAud{ 35u };
		constexpr uint8_t H265NalTypePrefixSei{ 39u };
		constexpr uint8_t H265NalTypeSuffixSei{ 40u };
		constexpr uint8_t H265NalTypeAp{ 48u };

		// A viewer that was handed a key frame but never confirmed receiving it
		// keeps asking as a first-frame requester; this budget bounds how much
		// publisher key-frame generation one unconfirmed episode may drive.
		constexpr uint32_t KeyFrameFirstFrameUnconfirmedMaxAsks{ 5u };
		// How long a viewer that already acknowledged the handed key frame
		// (RTCP Receiver Report, ring-2 evidence: "packets arrived") must keep
		// asking before the episode resolves as served. 2026-09-17 ZL92061/front
		// is ring 3 ("received but decoded nothing"): the viewer kept RR-acking
		// while still PLIing, so a bare ack must not close the episode. Only
		// "acked AND quiet for this long" counts as a served first frame.
		constexpr uint64_t KeyFrameFirstFrameUnconfirmedQuietMs{ 3000u };

		bool IsH264SyncParameterNalType(uint8_t nalType)
		{
			switch (nalType)
			{
				case H264NalTypeSps:
				case H264NalTypePps:
				case H264NalTypeSei:
				case H264NalTypeAud:
					return true;
				default:
					return false;
			}
		}

		uint8_t GetH265NalType(const uint8_t* payload)
		{
			return static_cast<uint8_t>((payload[0] >> 1) & 0x3F);
		}

		bool IsValidH265NalHeader(const uint8_t* payload)
		{
			constexpr uint8_t ForbiddenZeroBitMask{ 0x80u };
			constexpr uint8_t TemporalIdPlusOneMask{ 0x07u };

			if ((payload[0] & ForbiddenZeroBitMask) != 0u)
			{
				return false;
			}

			return (payload[1] & TemporalIdPlusOneMask) != 0u;
		}

		bool IsH265SyncParameterNalType(uint8_t nalType)
		{
			switch (nalType)
			{
				case H265NalTypeVps:
				case H265NalTypeSps:
				case H265NalTypePps:
				case H265NalTypeAud:
				case H265NalTypePrefixSei:
				case H265NalTypeSuffixSei:
					return true;
				default:
					return false;
			}
		}

		bool AllowsH264SyncParameterPacket(const uint8_t* payload, size_t payloadLength)
		{
			if (!payload || payloadLength < 1u)
			{
				return false;
			}

			const uint8_t nalType = payload[0] & 0x1F;

			if (IsH264SyncParameterNalType(nalType))
			{
				return true;
			}

			if (nalType != H264NalTypeStapA)
			{
				return false;
			}

			size_t offset{ 1u };
			size_t remaining = payloadLength - 1u;

			while (remaining >= 3u)
			{
				const auto naluSize = Utils::Byte::Get2Bytes(payload, offset);
				if (remaining < static_cast<size_t>(naluSize) + 2u)
				{
					break;
				}

				const uint8_t subNalType = payload[offset + 2] & 0x1F;
				if (IsH264SyncParameterNalType(subNalType))
				{
					return true;
				}

				offset += naluSize + 2u;
				remaining -= naluSize + 2u;
			}

			return false;
		}

		bool AllowsH265SyncParameterPacket(const uint8_t* payload, size_t payloadLength)
		{
			if (!payload || payloadLength < 2u || !IsValidH265NalHeader(payload))
			{
				return false;
			}

			const uint8_t nalType = GetH265NalType(payload);

			if (IsH265SyncParameterNalType(nalType))
			{
				return true;
			}

			if (nalType != H265NalTypeAp)
			{
				return false;
			}

			size_t offset{ 2u };
			size_t remaining = payloadLength - 2u;
			bool hasSyncParameterNal{ false };

			while (remaining >= 2u)
			{
				const auto naluSize = Utils::Byte::Get2Bytes(payload, offset);

				offset += 2u;
				remaining -= 2u;

				if (naluSize < 2u || remaining < static_cast<size_t>(naluSize))
				{
					return false;
				}

				if (!IsValidH265NalHeader(payload + offset))
				{
					return false;
				}

				const uint8_t subNalType = GetH265NalType(payload + offset);

				if (subNalType >= H265NalTypeAp)
				{
					return false;
				}

				if (IsH265SyncParameterNalType(subNalType))
				{
					hasSyncParameterNal = true;
				}

				offset += naluSize;
				remaining -= naluSize;
			}

			return hasSyncParameterNal && remaining == 0u;
		}

		bool AllowsSyncParameterPacket(
		  const RTC::RtpCodecMimeType& mimeType, const uint8_t* payload, size_t payloadLength)
		{
			if (mimeType.type != RTC::RtpCodecMimeType::Type::VIDEO)
			{
				return false;
			}

			switch (mimeType.subtype)
			{
				case RTC::RtpCodecMimeType::Subtype::H264:
					return AllowsH264SyncParameterPacket(payload, payloadLength);
				case RTC::RtpCodecMimeType::Subtype::H265:
					return AllowsH265SyncParameterPacket(payload, payloadLength);
				default:
					return false;
			}
		}

		uint8_t GetNalTypeForLog(
		  const RTC::RtpCodecMimeType& mimeType, const uint8_t* payload, size_t payloadLength)
		{
			if (!payload || payloadLength == 0u)
			{
				return 0u;
			}

			if (mimeType.subtype == RTC::RtpCodecMimeType::Subtype::H265 && payloadLength >= 2u)
			{
				return GetH265NalType(payload);
			}

			return payload[0] & 0x1F;
		}

		bool HasExactExtensionProfile(
		  const RTC::RtpPacket* packet,
		  const std::vector<RTC::RtpPacket::GenericExtension>& extensions,
		  const RTC::RtpHeaderExtensionIds& extensionIds)
		{
			if (packet->GetExtensionCount() != extensions.size())
			{
				return false;
			}

			// Equal id/length sets are not equivalent if same-sized URI values have
			// exchanged ids (for example MID/ABS or MID/TWCC).
			if (
			  packet->GetMidExtensionId() != extensionIds.mid ||
			  packet->GetAbsSendTimeExtensionId() != extensionIds.absSendTime ||
			  packet->GetAbsCaptureTimeExtensionId() != extensionIds.absCaptureTime ||
			  packet->GetTransportWideCc01ExtensionId() != extensionIds.transportWideCc01)
			{
				return false;
			}

			for (const auto& extension : extensions)
			{
				uint8_t currentLength{ 0u };
				if (!packet->GetExtension(extension.id, currentLength) || currentLength != extension.len)
				{
					return false;
				}
			}

			return true;
		}

		bool IsCompatibleRetransmissionPacket(
		  const RTC::RtpPacket* candidate,
		  const RTC::RtpPacket* source,
		  const std::vector<RTC::RtpPacket::GenericExtension>& extensions,
		  const RTC::RtpHeaderExtensionIds& extensionIds)
		{
			if (!candidate || !HasExactExtensionProfile(candidate, extensions, extensionIds))
			{
				return false;
			}

			if (
			  candidate->GetPayloadType() != source->GetPayloadType() ||
			  candidate->HasMarker() != source->HasMarker() ||
			  candidate->GetTimestamp() != source->GetTimestamp() ||
			  candidate->GetPayloadLength() != source->GetPayloadLength() ||
			  candidate->GetPayloadPadding() != source->GetPayloadPadding())
			{
				return false;
			}

			return candidate->GetPayloadLength() == 0u ||
			       std::memcmp(
			         candidate->GetPayload(), source->GetPayload(), source->GetPayloadLength()) == 0;
		}

		uint8_t SelectExtensionFormat(const std::vector<RTC::RtpPacket::GenericExtension>& extensions)
		{
			for (const auto& extension : extensions)
			{
				if (extension.id > 14u || extension.len > 16u)
				{
					return 2u;
				}
			}

			return 1u;
		}
	} // namespace

	/* Instance methods. */

	SimpleConsumer::SimpleConsumer(
	  RTC::Shared* shared,
	  const std::string& id,
	  const std::string& producerId,
	  RTC::Consumer::Listener* listener,
	  const FBS::Transport::ConsumeRequest* data)
	  : RTC::Consumer::Consumer(shared, id, producerId, listener, data, RTC::RtpParameters::Type::SIMPLE)
	{
		MS_TRACE();

		// Ensure there is a single encoding.
		if (this->consumableRtpEncodings.size() != 1u)
		{
			MS_THROW_TYPE_ERROR("invalid consumableRtpEncodings with size != 1");
		}
		if (this->rtpParameters.mid.size() > std::numeric_limits<uint8_t>::max())
		{
			MS_THROW_TYPE_ERROR(
			  "RTP MID exceeds encodable extension length [length:%zu, max:%" PRIu8 "]",
			  this->rtpParameters.mid.size(),
			  std::numeric_limits<uint8_t>::max());
		}

		auto& encoding         = this->rtpParameters.encodings[0];
		const auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		this->keyFrameSupported = RTC::Codecs::Tools::CanBeKeyFrame(mediaCodec->mimeType);

		// Create RtpStreamSend instance for sending a single stream to the remote.
		CreateRtpStream();

		// Create the encoding context for Opus.
		if (
		  mediaCodec->mimeType.type == RTC::RtpCodecMimeType::Type::AUDIO &&
		  (mediaCodec->mimeType.subtype == RTC::RtpCodecMimeType::Subtype::OPUS ||
		   mediaCodec->mimeType.subtype == RTC::RtpCodecMimeType::Subtype::MULTIOPUS))
		{
			RTC::Codecs::EncodingContext::Params params;

			this->encodingContext.reset(
			  RTC::Codecs::Tools::GetEncodingContext(mediaCodec->mimeType, params));

			// ignoreDtx is set to false by default.
			this->encodingContext->SetIgnoreDtx(data->ignoreDtx());
		}

		// NOTE: This may throw.
		this->shared->channelMessageRegistrator->RegisterHandler(
		  this->id,
		  /*channelRequestHandler*/ this,
		  /*channelNotificationHandler*/ nullptr);
	}

	SimpleConsumer::~SimpleConsumer()
	{
		MS_TRACE();

		this->shared->channelMessageRegistrator->UnregisterHandler(this->id);

		delete this->rtpStream;
	}

	flatbuffers::Offset<FBS::Consumer::DumpResponse> SimpleConsumer::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		// Call the parent method.
		auto base = RTC::Consumer::FillBuffer(builder);
		// Add rtpStream.
		std::vector<flatbuffers::Offset<FBS::RtpStream::Dump>> rtpStreams;
		rtpStreams.emplace_back(this->rtpStream->FillBuffer(builder));

		auto dump = FBS::Consumer::CreateConsumerDumpDirect(builder, base, &rtpStreams);

		return FBS::Consumer::CreateDumpResponse(builder, dump);
	}

	flatbuffers::Offset<FBS::Consumer::GetStatsResponse> SimpleConsumer::FillBufferStats(
	  flatbuffers::FlatBufferBuilder& builder)
	{
		MS_TRACE();

		std::vector<flatbuffers::Offset<FBS::RtpStream::Stats>> rtpStreams;

		// Add stats of our send stream.
		rtpStreams.emplace_back(this->rtpStream->FillBufferStats(builder));

		// Add stats of our recv stream.
		if (this->producerRtpStream)
		{
			rtpStreams.emplace_back(this->producerRtpStream->FillBufferStats(builder));
		}

		return FBS::Consumer::CreateGetStatsResponseDirect(builder, &rtpStreams);
	}

	flatbuffers::Offset<FBS::Consumer::ConsumerScore> SimpleConsumer::FillBufferScore(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		MS_TRACE();

		MS_ASSERT(this->producerRtpStreamScores, "producerRtpStreamScores not set");

		uint8_t producerScore{ 0 };

		if (this->producerRtpStream)
		{
			producerScore = this->producerRtpStream->GetScore();
		}

		return FBS::Consumer::CreateConsumerScoreDirect(
		  builder, this->rtpStream->GetScore(), producerScore, this->producerRtpStreamScores);
	}

	void SimpleConsumer::HandleRequest(Channel::ChannelRequest* request)
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

	void SimpleConsumer::ProducerRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t /*mappedSsrc*/)
	{
		MS_TRACE();

		this->producerRtpStream = rtpStream;
	}

	void SimpleConsumer::ProducerNewRtpStream(RTC::RtpStreamRecv* rtpStream, uint32_t /*mappedSsrc*/)
	{
		MS_TRACE();

		this->producerRtpStream = rtpStream;

		// Emit the score event.
		EmitScore();
	}

	void SimpleConsumer::ProducerRtpStreamScore(
	  RTC::RtpStreamRecv* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/)
	{
		MS_TRACE();

		// Emit the score event.
		EmitScore();
	}

	void SimpleConsumer::ProducerRtcpSenderReport(RTC::RtpStreamRecv* /*rtpStream*/, bool /*first*/)
	{
		MS_TRACE();

		// Do nothing.
	}

	uint8_t SimpleConsumer::GetBitratePriority() const
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		// Audio SimpleConsumer does not play the BWE game.
		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return 0u;
		}

		if (!IsActive())
		{
			return 0u;
		}

		return this->priority;
	}

	uint32_t SimpleConsumer::IncreaseLayer(uint32_t bitrate, bool /*considerLoss*/)
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");
		MS_ASSERT(this->kind == RTC::Media::Kind::VIDEO, "should be video");
		MS_ASSERT(IsActive(), "should be active");

		// If this is not the first time this method is called within the same iteration,
		// return 0 since a video SimpleConsumer does not keep state about this.
		if (this->managingBitrate)
		{
			return 0u;
		}

		this->managingBitrate = true;

		// Video SimpleConsumer does not really play the BWE game when. However, let's
		// be honest and try to be nice.
		auto nowMs          = DepLibUV::GetTimeMs();
		auto desiredBitrate = this->producerRtpStream->GetBitrate(nowMs);

		if (desiredBitrate < bitrate)
		{
			return desiredBitrate;
		}
		else
		{
			return bitrate;
		}
	}

	void SimpleConsumer::ApplyLayers()
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");
		MS_ASSERT(this->kind == RTC::Media::Kind::VIDEO, "should be video");
		MS_ASSERT(IsActive(), "should be active");

		this->managingBitrate = false;

		// SimpleConsumer does not play the BWE game (even if video kind).
	}

	uint32_t SimpleConsumer::GetDesiredBitrate() const
	{
		MS_TRACE();

		MS_ASSERT(this->externallyManagedBitrate, "bitrate is not externally managed");

		// Audio SimpleConsumer does not play the BWE game.
		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return 0u;
		}

		if (!IsActive())
		{
			return 0u;
		}

		auto nowMs          = DepLibUV::GetTimeMs();
		auto desiredBitrate = this->producerRtpStream->GetBitrate(nowMs);

		// If consumer.rtpParameters.encodings[0].maxBitrate was given and it's
		// greater than computed one, then use it.
		auto maxBitrate = this->rtpParameters.encodings[0].maxBitrate;

		if (maxBitrate > desiredBitrate)
		{
			desiredBitrate = maxBitrate;
		}

		return desiredBitrate;
	}

	void SimpleConsumer::SendRtpPacket(
	  RTC::RtpPacket* packet, RTC::Consumer::RtpPacketFanoutContext& fanoutContext)
	{
		MS_TRACE();
		RTC::Consumer::RtpPacketMutationGuard canonicalPacketGuard(packet, true);

#ifdef MS_RTC_LOGGER_RTP
		packet->logger.consumerId = this->id;
#endif

		if (!IsActive())
		{
			MS_DEBUG_DEV(
			  "simple consumer inactive, dropping packet [consumerId:%s, producerId:%s, seq:%" PRIu16 "]",
			  this->id.c_str(),
			  this->producerId.c_str(),
			  packet->GetSequenceNumber());
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
			MS_DEBUG_DEV(
			  "simple consumer payload type not supported [consumerId:%s, producerId:%s, payloadType:%" PRIu8
			  "]",
			  this->id.c_str(),
			  this->producerId.c_str(),
			  payloadType);

#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::UNSUPPORTED_PAYLOAD_TYPE);
#endif

			return;
		}

		// SimpleConsumer only creates an encoding context for Opus/MultiOpus. That
		// handler can reject DTX but never mutates payload bytes, so perform the drop
		// decision before any extension clone/allocation.
		bool marker{ false };

		if (this->encodingContext && !packet->ProcessPayload(this->encodingContext.get(), marker))
		{
			MS_DEBUG_DEV(
			  "discarding packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp());
			MS_DEBUG_DEV(
			  "simple consumer codec processing dropped packet [consumerId:%s, producerId:%s, seq:%" PRIu16
			  ", ts:%" PRIu32 "]",
			  this->id.c_str(),
			  this->producerId.c_str(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp());

			this->rtpSeqManager.Drop(packet->GetSequenceNumber());

#ifdef MS_RTC_LOGGER_RTP
			packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::DROPPED_BY_CODEC);
#endif

			return;
		}

		// Drop non-sync video before building any consumer-specific extension
		// profile. Parameter packets intentionally pass through while waiting for
		// the first keyframe.
		if (this->syncRequired && this->keyFrameSupported && !packet->IsKeyFrame())
		{
			auto& encoding           = this->rtpParameters.encodings[0];
			const auto* mediaCodec   = this->rtpParameters.GetCodecForEncoding(encoding);
			const auto* payload      = packet->GetPayload();
			const auto payloadLength = packet->GetPayloadLength();
			const bool allowSyncParameterPacket =
			  mediaCodec && AllowsSyncParameterPacket(mediaCodec->mimeType, payload, payloadLength);
			[[maybe_unused]] const uint8_t nalType =
			  mediaCodec ? GetNalTypeForLog(mediaCodec->mimeType, payload, payloadLength) : 0u;

			if (allowSyncParameterPacket)
			{
				MS_DEBUG_DEV(
				  "simple consumer forwarding codec sync parameter packet while waiting for keyframe "
				  "[consumerId:%s, producerId:%s, codec:%s, seq:%" PRIu16 ", ts:%" PRIu32
				  ", nalType:%" PRIu8 "]",
				  this->id.c_str(),
				  this->producerId.c_str(),
				  mediaCodec->mimeType.ToString().c_str(),
				  packet->GetSequenceNumber(),
				  packet->GetTimestamp(),
				  nalType);
			}
			else
			{
				MS_DEBUG_DEV(
				  "simple consumer waiting for keyframe [consumerId:%s, producerId:%s, seq:%" PRIu16
				  ", ts:%" PRIu32 ", nalType:%" PRIu8 ", isKeyFrame:%s]",
				  this->id.c_str(),
				  this->producerId.c_str(),
				  packet->GetSequenceNumber(),
				  packet->GetTimestamp(),
				  nalType,
				  packet->IsKeyFrame() ? "true" : "false");
#ifdef MS_RTC_LOGGER_RTP
				packet->logger.Dropped(RtcLogger::RtpPacket::DropReason::NOT_A_KEYFRAME);
#endif

				return;
			}
		}

		// A SimpleConsumer can negotiate a different outbound extension layout than
		// other Consumers fed by the same Router packet. Keep one immutable clone per
		// compatible profile for this fanout, while preserving the ingress packet for
		// later consumers and RTP observers.
		std::shared_ptr<RTC::RtpPacket> outboundPacket;
		thread_local static uint8_t extensionBuffer[512];
		thread_local static std::vector<RTC::RtpPacket::GenericExtension> extensions;

		if (extensions.capacity() != 16)
		{
			extensions.reserve(16);
		}

		extensions.clear();

		uint8_t* extensionBufferPtr{ extensionBuffer };

		// MID.
		if (this->rtpHeaderExtensionIds.mid != 0u && !this->rtpParameters.mid.empty())
		{
			const auto extensionLength = static_cast<uint8_t>(this->rtpParameters.mid.size());
			std::memcpy(extensionBufferPtr, this->rtpParameters.mid.data(), extensionLength);
			extensions.emplace_back(this->rtpHeaderExtensionIds.mid, extensionLength, extensionBufferPtr);
			extensionBufferPtr += extensionLength;
		}

		// Absolute Capture Time.
		if (this->rtpHeaderExtensionIds.absCaptureTime != 0u)
		{
			uint8_t extensionLength{ 0u };
			auto* sourceValue = packet->GetExtension(
			  packet->GetAbsCaptureTimeExtensionId(),
			  extensionLength);

			if (sourceValue && (extensionLength == 8u || extensionLength == 16u))
			{
				std::memcpy(extensionBufferPtr, sourceValue, extensionLength);
				extensions.emplace_back(
				  this->rtpHeaderExtensionIds.absCaptureTime,
				  extensionLength,
				  extensionBufferPtr);
				extensionBufferPtr += extensionLength;
			}
		}

		// abs-send-time.
		if (this->rtpHeaderExtensionIds.absSendTime != 0u)
		{
			const uint8_t extensionLength = 3u;
			Utils::Byte::Set3Bytes(extensionBufferPtr, 0, 0u);
			extensions.emplace_back(
			  this->rtpHeaderExtensionIds.absSendTime, extensionLength, extensionBufferPtr);
			extensionBufferPtr += extensionLength;
		}

		// transport-wide-cc.
		if (this->rtpHeaderExtensionIds.transportWideCc01 != 0u)
		{
			const uint8_t extensionLength = 2u;
			Utils::Byte::Set2Bytes(extensionBufferPtr, 0, 0u);
			extensions.emplace_back(
			  this->rtpHeaderExtensionIds.transportWideCc01, extensionLength, extensionBufferPtr);
		}

		const bool requiresExtensionRewrite =
		  !HasExactExtensionProfile(packet, extensions, this->rtpHeaderExtensionIds);

		if (requiresExtensionRewrite)
		{
			for (const auto& candidate : fanoutContext.simpleConsumerProfiles)
			{
				if (IsCompatibleRetransmissionPacket(
				      candidate.get(), packet, extensions, this->rtpHeaderExtensionIds))
				{
					outboundPacket = candidate;
					break;
				}
			}

			if (!outboundPacket)
			{
				outboundPacket.reset(packet->Clone());
				auto* rewrittenPacket = outboundPacket.get();

				if (!rewrittenPacket->SetExtensions(SelectExtensionFormat(extensions), extensions))
				{
					MS_WARN_TAG(
					  rtp,
					  "dropping RTP packet with insufficient extension capacity "
					  "[consumerId:%s, producerId:%s, size:%zu]",
					  this->id.c_str(),
					  this->producerId.c_str(),
					  rewrittenPacket->GetSize());
					return;
				}

				rewrittenPacket->SetMidExtensionId(this->rtpHeaderExtensionIds.mid);
				rewrittenPacket->SetAbsCaptureTimeExtensionId(
				  this->rtpHeaderExtensionIds.absCaptureTime);
				rewrittenPacket->SetAbsSendTimeExtensionId(this->rtpHeaderExtensionIds.absSendTime);
				rewrittenPacket->SetTransportWideCc01ExtensionId(
				  this->rtpHeaderExtensionIds.transportWideCc01);

				// Caching is an optimization only. If the profile-index allocation fails,
				// this consumer can still safely send and retain its owned packet.
				try
				{
					fanoutContext.simpleConsumerProfiles.emplace_back(outboundPacket);
				}
				catch (const std::bad_alloc&)
				{
				}
			}

			packet = outboundPacket.get();

			if (this->rtpHeaderExtensionIds.mid != 0u)
			{
				if (!packet->UpdateMid(this->rtpParameters.mid))
				{
					MS_WARN_TAG(
					  rtp,
					  "dropping RTP packet because MID cannot be rewritten [consumerId:%s]",
					  this->id.c_str());
					return;
				}
			}
			if (this->rtpHeaderExtensionIds.absSendTime != 0u)
			{
				packet->UpdateAbsSendTime(0u);
			}
			if (this->rtpHeaderExtensionIds.transportWideCc01 != 0u)
			{
				packet->UpdateTransportWideCc01(0u);
			}

#ifdef MS_RTC_LOGGER_RTP
			packet->logger.consumerId = this->id;
#endif
		}
		else
		{
			// Keep the compatible shared profile canonical for every consumer. The
			// Router restores these in-place values after the send, while the lazy
			// retransmission clone retains the consumer MID and neutral transport
			// timestamps for its own RtpStreamSend.
			if (this->rtpHeaderExtensionIds.mid != 0u)
			{
				if (!packet->UpdateMid(this->rtpParameters.mid))
				{
					MS_WARN_TAG(
					  rtp,
					  "dropping RTP packet because MID cannot be rewritten [consumerId:%s]",
					  this->id.c_str());
					return;
				}
			}
			if (this->rtpHeaderExtensionIds.absSendTime != 0u)
			{
				packet->UpdateAbsSendTime(0u);
			}
			if (this->rtpHeaderExtensionIds.transportWideCc01 != 0u)
			{
				packet->UpdateTransportWideCc01(0u);
			}
		}

		// Whether this is the first packet after re-sync.
		const bool isSyncPacket =
		  this->syncRequired && (!this->keyFrameSupported || packet->IsKeyFrame());

		// Sync sequence number and timestamp if required.
		if (isSyncPacket)
		{
			if (packet->IsKeyFrame())
			{
				MS_DEBUG_TAG(rtp, "sync key frame received");
			}

			this->rtpSeqManager.Sync(packet->GetSequenceNumber() - 1);

			this->syncRequired = false;
		}

		// Update RTP seq number and timestamp.
		uint16_t seq;

		this->rtpSeqManager.Input(packet->GetSequenceNumber(), seq);

		// Save original packet fields.
		auto origSeq = packet->GetSequenceNumber();
		RTC::Consumer::RtpPacketMutationGuard outboundPacketGuard(packet);

		// Rewrite packet.
		packet->SetSsrc(this->rtpParameters.encodings[0].ssrc);
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
			  "] from original [seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSeq);
		}

		// Process the packet. The default lazy clone may have been produced by a
		// payload-rewriting SVC/Simulcast consumer earlier in this fanout. Never let
		// SimpleConsumer retain it unless both the extension profile and immutable
		// payload match the packet sent here.
		auto& retransmissionPacket = outboundPacket ? outboundPacket : fanoutContext.sharedPacket;

		if (
		  !outboundPacket && retransmissionPacket &&
		  !IsCompatibleRetransmissionPacket(
		    retransmissionPacket.get(), packet, extensions, this->rtpHeaderExtensionIds))
		{
			retransmissionPacket.reset();
		}

		if (this->rtpStream->ReceivePacket(packet, retransmissionPacket))
		{
			MS_DEBUG_DEV(
			  "simple consumer sending RTP [consumerId:%s, producerId:%s, seq:%" PRIu16 ", ts:%" PRIu32
			  ", marker:%s, keyframe:%s]",
			  this->id.c_str(),
			  this->producerId.c_str(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  packet->HasMarker() ? "true" : "false",
			  packet->IsKeyFrame() ? "true" : "false");
			// Send the packet.
			this->listener->OnConsumerSendRtpPacket(this, packet);

			// May emit 'trace' event.
			EmitTraceEventRtpAndKeyFrameTypes(packet);

			// Downlink key-frame handoff evidence (rate-limited, production INFO on the
			// evidence channel): lets triage prove whether SimpleConsumer handed a
			// key frame to the transport, independent of whether the browser is
			// still reporting its receive/decode counters.
			if (packet->IsKeyFrame())
			{
				++this->keyFramesEmitted;
				const uint64_t nowMs = DepLibUV::GetTimeMs();

				// Record the handed sync key frame in the viewer's own sequence
				// domain: an RTCP Receiver Report acknowledges extended (32-bit)
				// sequence numbers, so storing the plain 16-bit wire sequence
				// here would break the confirmation check after the first 64K
				// sequence wrap. RtpStream has just counted this packet's cycle.
				if (isSyncPacket)
				{
					this->syncKeyFrameHanded = true;
					this->syncKeyFrameSeq    = this->rtpStream->GetExtendedSeq(seq);
				}

				if (nowMs - this->lastKeyFrameEvidenceAtMs >= 10000u)
				{
					this->lastKeyFrameEvidenceAtMs = nowMs;
					MS_EVIDENCE_INFO(
					  "downlink key frame handed to transport [consumerId:%s, producerId:%s, keyFramesEmitted:%" PRIu32
					  ", viewerHighestSeqReceived:%" PRIu32 ", viewerFractionLost:%" PRIu8 "]",
					  this->id.c_str(),
					  this->producerId.c_str(),
					  this->keyFramesEmitted,
					  this->rtpStream->GetRtcpHighestSeqReceived(),
					  this->rtpStream->GetFractionLost());
				}
			}
		}
		else
		{
			MS_WARN_TAG(
			  rtp,
			  "failed to send packet [ssrc:%" PRIu32 ", seq:%" PRIu16 ", ts:%" PRIu32
			  "] from original [seq:%" PRIu16 "]",
			  packet->GetSsrc(),
			  packet->GetSequenceNumber(),
			  packet->GetTimestamp(),
			  origSeq);
		}

	}

	bool SimpleConsumer::GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t nowMs)
	{
		MS_TRACE();

		if (static_cast<float>((nowMs - this->lastRtcpSentTime) * 1.15) < this->maxRtcpInterval)
		{
			return true;
		}

		auto* senderReport = this->rtpStream->GetRtcpSenderReport(nowMs);

		if (!senderReport)
		{
			return true;
		}

		// Build SDES chunk for this sender.
		auto* sdesChunk = this->rtpStream->GetRtcpSdesChunk();

		auto* delaySinceLastRrSsrcInfo = this->rtpStream->GetRtcpXrDelaySinceLastRrSsrcInfo(nowMs);

		// RTCP Compound packet buffer cannot hold the data.
		if (!packet->Add(senderReport, sdesChunk, delaySinceLastRrSsrcInfo))
		{
			return false;
		}

		this->lastRtcpSentTime = nowMs;

		return true;
	}

	void SimpleConsumer::NeedWorstRemoteFractionLost(
	  uint32_t /*mappedSsrc*/, uint8_t& worstRemoteFractionLost)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return;
		}

		auto fractionLost = this->rtpStream->GetFractionLost();

		// If our fraction lost is worse than the given one, update it.
		if (fractionLost > worstRemoteFractionLost)
		{
			worstRemoteFractionLost = fractionLost;
		}
	}

	void SimpleConsumer::ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return;
		}

		// May emit 'trace' event.
		EmitTraceEventNackType();

		this->rtpStream->ReceiveNack(nackPacket);
	}

	void SimpleConsumer::ReceiveKeyFrameRequest(
	  RTC::RTCP::FeedbackPs::MessageType messageType, uint32_t ssrc)
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

		this->rtpStream->ReceiveKeyFrameRequest(messageType);

		if (IsActive())
		{
			RequestKeyFrame(/*fromViewerRtcp=*/true);
		}
	}

	void SimpleConsumer::ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();

		this->rtpStream->ReceiveRtcpReceiverReport(report);

		// A Receiver Report can resolve an unconfirmed first-frame episode once
		// the viewer has both confirmed the handed sequence and been quiet long
		// enough; running the check here also keeps the unconfirmed flag from
		// lingering until the viewer's next ask (round-2 review R14.1/14.2).
		if (this->firstFrameUnconfirmed)
		{
			ResolveFirstFrameConfirmation();
		}
	}

	void SimpleConsumer::ReceiveRtcpXrReceiverReferenceTime(RTC::RTCP::ReceiverReferenceTime* report)
	{
		MS_TRACE();

		this->rtpStream->ReceiveRtcpXrReceiverReferenceTime(report);
	}

	uint32_t SimpleConsumer::GetTransmissionRate(uint64_t nowMs)
	{
		MS_TRACE();

		if (!IsActive())
		{
			return 0u;
		}

		return this->rtpStream->GetBitrate(nowMs);
	}

	float SimpleConsumer::GetRtt() const
	{
		MS_TRACE();

		return this->rtpStream->GetRtt();
	}

	void SimpleConsumer::UserOnTransportConnected()
	{
		MS_TRACE();

		this->syncRequired = true;
		MarkFirstFrameUnconfirmed();

		if (IsActive())
		{
			RequestKeyFrame();
		}
	}

	void SimpleConsumer::UserOnTransportDisconnected()
	{
		MS_TRACE();

		this->rtpStream->Pause();
	}

	void SimpleConsumer::UserOnPaused()
	{
		MS_TRACE();

		this->rtpStream->Pause();

		if (this->externallyManagedBitrate && this->kind == RTC::Media::Kind::VIDEO)
		{
			this->listener->OnConsumerNeedZeroBitrate(this);
		}
	}

	void SimpleConsumer::UserOnResumed()
	{
		MS_TRACE();

		this->syncRequired = true;
		MarkFirstFrameUnconfirmed();

		if (IsActive())
		{
			RequestKeyFrame();
		}
	}

	void SimpleConsumer::CreateRtpStream()
	{
		MS_TRACE();

		auto& encoding         = this->rtpParameters.encodings[0];
		const auto* mediaCodec = this->rtpParameters.GetCodecForEncoding(encoding);

		MS_DEBUG_TAG(
		  rtp, "[ssrc:%" PRIu32 ", payloadType:%" PRIu8 "]", encoding.ssrc, mediaCodec->payloadType);

		// Set stream params.
		RTC::RtpStream::Params params;

		params.ssrc        = encoding.ssrc;
		params.payloadType = mediaCodec->payloadType;
		params.mimeType    = mediaCodec->mimeType;
		params.clockRate   = mediaCodec->clockRate;
		params.cname       = this->rtpParameters.rtcp.cname;

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

		this->rtpStream = new RTC::RtpStreamSend(this, params, this->rtpParameters.mid);
		this->rtpStreams.push_back(this->rtpStream);

		// If the Consumer is paused, tell the RtpStreamSend.
		if (IsPaused() || IsProducerPaused())
		{
			this->rtpStream->Pause();
		}

		const auto* rtxCodec = this->rtpParameters.GetRtxCodecForEncoding(encoding);

		if (rtxCodec && encoding.hasRtx)
		{
			this->rtpStream->SetRtx(rtxCodec->payloadType, encoding.rtx.ssrc);
		}
	}

	void SimpleConsumer::MarkFirstFrameUnconfirmed()
	{
		this->firstFrameUnconfirmed            = true;
		this->firstFrameUnconfirmedAsks        = 0u;
		this->firstFrameUnconfirmedSinceMs     = DepLibUV::GetTimeMs();
		// Seeded to 0 ("no viewer ask yet"): an episode whose viewer has never
		// asked must not be resolved as quiet on the first RR that acks the
		// handoff -- the viewer's very next PLI may already be in flight
		// (round-2 review R8, ack-before-PLI ordering).
		this->firstFrameUnconfirmedLastAskMs   = 0u;
		// A key frame handed before this point cannot confirm a new episode.
		this->syncKeyFrameHanded = false;
		this->syncKeyFrameSeq    = 0u;
	}

	void SimpleConsumer::ResolveFirstFrameConfirmation()
	{
		const uint64_t nowMs = DepLibUV::GetTimeMs();
		const uint32_t ackedSeq =
		  this->rtpStream != nullptr ? this->rtpStream->GetRtcpHighestSeqReceived() : 0u;
		// The viewer confirms by acknowledging (RTCP Receiver Report) the very
		// sequence that carried the sync key frame to its transport.
		const bool viewerConfirmed = this->syncKeyFrameHanded && ackedSeq >= this->syncKeyFrameSeq;
		const bool budgetSpent = this->firstFrameUnconfirmedAsks >= KeyFrameFirstFrameUnconfirmedMaxAsks;
		// An RTCP RR ack alone is ring-2 evidence ("packets arrived") and must
		// not close the episode: the documented 2026-09-17 ZL92061/front shape
		// is ring 3, a viewer that acked the handed key frame yet decoded
		// nothing and kept PLIing. The episode resolves as served only once the
		// viewer has confirmed the handed sequence AND has raised at least one
		// viewer ask AND then fallen quiet for a window; a viewer that acks
		// before its first PLI must not be resolved as quiet (the PLI may be in
		// flight). The ask budget still bounds viewers that never RR.
		const bool quietAfterConfirm =
		  viewerConfirmed && this->firstFrameUnconfirmedLastAskMs != 0u &&
		  (nowMs - this->firstFrameUnconfirmedLastAskMs) >= KeyFrameFirstFrameUnconfirmedQuietMs;

		if (!budgetSpent && !quietAfterConfirm)
		{
			return;
		}

		this->firstFrameUnconfirmed = false;

		MS_EVIDENCE_INFO(
		  "consumer first-frame confirmation resolved [consumerId:%s, producerId:%s, syncKeyFrameSeq:%" PRIu32
		  ", viewerHighestSeqReceived:%" PRIu32 ", asks:%" PRIu32 ", unconfirmedMs:%" PRIu64
		  ", lastAskAgoMs:%" PRIu64 ", reason:%s]",
		  this->id.c_str(),
		  this->producerId.c_str(),
		  this->syncKeyFrameSeq,
		  ackedSeq,
		  this->firstFrameUnconfirmedAsks,
		  nowMs - this->firstFrameUnconfirmedSinceMs,
		  nowMs - this->firstFrameUnconfirmedLastAskMs,
		  budgetSpent ? "ask-budget-spent" : "viewer-ack-quiet");
	}

	void SimpleConsumer::RequestKeyFrame(bool fromViewerRtcp, bool firstFrameRequest)
	{
		MS_TRACE();

		if (this->kind != RTC::Media::Kind::VIDEO)
		{
			return;
		}

		auto mappedSsrc = this->consumableRtpEncodings[0].ssrc;

		// A handed key frame is not a rendered frame. While the handoff of the
		// current episode stays unacknowledged, this consumer is a first-frame
		// requester even though syncRequired was already cleared by the handoff:
		// otherwise a viewer whose first key frame never became decodable media
		// waits out a whole producer key-frame cadence (2026-09-17 ZL92061/front:
		// 5.0s first picture, every ask suppressed).
		//
		// A consumer that still needs its very first decodable frame asks right
		// away.  syncRequired is authoritative for "this consumer cannot render
		// anything until the next key frame": it is set on create / transport
		// (re)connect / resume and cleared only when a sync key frame is
		// actually received.  One signal covers first-frame, reconnect and
		// resume, replacing the old sticky firstKeyFrameDelivered flag;
		// firstFrameUnconfirmed additionally covers "handed but never confirmed".
		//
		// The effective flag is evaluated from the state BEFORE this ask may
		// resolve the episode: the ask that closes an unconfirmed handoff is
		// itself a first-frame ask. A viewer that acknowledges the handed key
		// frame (RTCP RR) while still PLIing is exactly "received but not
		// decoded", i.e. the one viewer that needs a forced key frame most;
		// resolving first would demote that ask to a regular one and let the
		// producer viewer-suppression path swallow it (round-2 review R9).
		const bool effectiveFirstFrame =
		  firstFrameRequest || this->syncRequired || this->firstFrameUnconfirmed;

		if (this->firstFrameUnconfirmed)
		{
			// Only a viewer-originated ask proves the viewer is still waiting
			// for a decodable frame and therefore refreshes the quiet window;
			// internal asks (signaling / connect / resume) do not.
			if (fromViewerRtcp)
			{
				this->firstFrameUnconfirmedLastAskMs = DepLibUV::GetTimeMs();
			}

			++this->firstFrameUnconfirmedAsks;
			ResolveFirstFrameConfirmation();
		}

		this->listener->OnConsumerKeyFrameRequested(this, mappedSsrc, fromViewerRtcp, effectiveFirstFrame);
	}

	inline void SimpleConsumer::EmitScore() const
	{
		MS_TRACE();

		auto scoreOffset = FillBufferScore(this->shared->channelNotifier->GetBufferBuilder());

		auto notificationOffset = FBS::Consumer::CreateScoreNotification(
		  this->shared->channelNotifier->GetBufferBuilder(), scoreOffset);

		this->shared->channelNotifier->Emit(
		  this->id,
		  FBS::Notification::Event::CONSUMER_SCORE,
		  FBS::Notification::Body::Consumer_ScoreNotification,
		  notificationOffset);
	}

	inline void SimpleConsumer::OnRtpStreamScore(
	  RTC::RtpStream* /*rtpStream*/, uint8_t /*score*/, uint8_t /*previousScore*/)
	{
		MS_TRACE();

		// Emit the score event.
		EmitScore();
	}

	inline void SimpleConsumer::OnRtpStreamRetransmitRtpPacket(
	  RTC::RtpStreamSend* /*rtpStream*/, RTC::RtpPacket* packet)
	{
		MS_TRACE();

		this->listener->OnConsumerRetransmitRtpPacket(this, packet);

		// May emit 'trace' event.
		EmitTraceEventRtpAndKeyFrameTypes(packet, this->rtpStream->HasRtx());
	}
} // namespace RTC

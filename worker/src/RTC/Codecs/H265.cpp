#define MS_CLASS "RTC::Codecs::H265"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/Codecs/H265.hpp"
#include "Logger.hpp"
#include "Utils.hpp"
#include <memory>

namespace RTC
{
	namespace Codecs
	{
		namespace
		{
			constexpr uint8_t NalTypeAp{ 48 };
			constexpr uint8_t NalTypeFu{ 49 };
			constexpr uint8_t NalTypePaci{ 50 };

			uint8_t GetNalType(const uint8_t* data)
			{
				return (data[0] >> 1) & 0x3F;
			}

			bool IsValidNalHeader(const uint8_t* data)
			{
				constexpr uint8_t ForbiddenZeroBitMask{ 0x80 };
				constexpr uint8_t TemporalIdPlusOneMask{ 0x07 };

				if ((data[0] & ForbiddenZeroBitMask) != 0)
				{
					return false;
				}

				return (data[1] & TemporalIdPlusOneMask) != 0;
			}

			bool IsIrapNalType(uint8_t nalType)
			{
				return nalType >= 16 && nalType <= 21;
			}

			void RecordParameterSet(H265::PayloadDescriptor& descriptor, uint8_t nalType)
			{
				switch (nalType)
				{
					case 32:
						descriptor.hasVps = true;
						break;
					case 33:
						descriptor.hasSps = true;
						break;
					case 34:
						descriptor.hasPps = true;
						break;
					default:
						break;
				}
			}

			bool ParseSingleNalUnit(H265::PayloadDescriptor& descriptor, const uint8_t* data)
			{
				const uint8_t nalType = GetNalType(data);
				if (nalType >= NalTypeAp)
				{
					return false;
				}

				RecordParameterSet(descriptor, nalType);
				if (IsIrapNalType(nalType))
				{
					descriptor.isKeyFrame = true;
				}
				return true;
			}

			bool ParseAggregationPacket(H265::PayloadDescriptor& descriptor, const uint8_t* data, size_t len)
			{
				size_t offset{ 2 };
				bool parsedNalUnit{ false };

				while (offset + 2 <= len)
				{
					const uint16_t naluSize = Utils::Byte::Get2Bytes(data, offset);
					offset += 2;

					if (naluSize < 2 || offset + naluSize > len)
					{
						MS_WARN_DEV("ignoring malformed H265 aggregation packet");

						return false;
					}
					if (!IsValidNalHeader(data + offset))
					{
						MS_WARN_DEV("ignoring H265 aggregation packet with invalid NAL unit header");

						return false;
					}

					const uint8_t nalType = GetNalType(data + offset);
					if (nalType >= NalTypeAp)
					{
						MS_WARN_DEV("ignoring H265 aggregation packet with nested packetization NAL type");

						return false;
					}
					RecordParameterSet(descriptor, nalType);
					if (IsIrapNalType(nalType))
					{
						descriptor.isKeyFrame = true;
					}

					parsedNalUnit = true;
					offset += naluSize;
				}

				return parsedNalUnit && offset == len;
			}

			bool ParseFragmentationUnit(H265::PayloadDescriptor& descriptor, const uint8_t* data, size_t len)
			{
				if (len < 3)
				{
					MS_WARN_DEV("ignoring malformed H265 fragmentation unit");

					return false;
				}

				const uint8_t fuHeader = data[2];
				const bool start       = (fuHeader & 0x80) != 0;
				const bool end         = (fuHeader & 0x40) != 0;
				if (start && end)
				{
					MS_WARN_DEV("ignoring H265 fragmentation unit with both start and end bits set");

					return false;
				}

				const uint8_t fuType = fuHeader & 0x3F;
				if (fuType >= NalTypeAp)
				{
					MS_WARN_DEV("ignoring H265 fragmentation unit with invalid FU type");

					return false;
				}

				RecordParameterSet(descriptor, fuType);
				if (start && IsIrapNalType(fuType))
				{
					descriptor.isKeyFrame = true;
				}
				return true;
			}
		} // namespace

		/* Class methods. */

		H265::PayloadDescriptor* H265::Parse(
		  const uint8_t* data, size_t len, RTC::RtpPacket::FrameMarking* frameMarking, uint8_t frameMarkingLen)
		{
			MS_TRACE();

			if (len < 2)
			{
				MS_WARN_DEV("ignoring payload with length < 2");

				return nullptr;
			}
			if (!IsValidNalHeader(data))
			{
				MS_WARN_DEV("ignoring payload with invalid H265 NAL unit header");

				return nullptr;
			}

			std::unique_ptr<PayloadDescriptor> payloadDescriptor(new PayloadDescriptor());

			if (frameMarking)
			{
				payloadDescriptor->s   = frameMarking->start;
				payloadDescriptor->e   = frameMarking->end;
				payloadDescriptor->i   = frameMarking->independent;
				payloadDescriptor->d   = frameMarking->discardable;
				payloadDescriptor->b   = frameMarking->base;
				payloadDescriptor->tid = frameMarking->tid;

				payloadDescriptor->hasTid = true;

				if (frameMarkingLen >= 2)
				{
					payloadDescriptor->hasLid = true;
					payloadDescriptor->lid    = frameMarking->lid;
				}

				if (frameMarkingLen == 3)
				{
					payloadDescriptor->hasTl0picidx = true;
					payloadDescriptor->tl0picidx    = frameMarking->tl0picidx;
				}

				if (frameMarking->start && frameMarking->independent)
				{
					payloadDescriptor->isKeyFrame = true;
				}
			}

			const uint8_t nalType = GetNalType(data);
			bool parsed           = false;

			switch (nalType)
			{
				case NalTypeAp:
					parsed = ParseAggregationPacket(*payloadDescriptor, data, len);
					break;

				case NalTypeFu:
					parsed = ParseFragmentationUnit(*payloadDescriptor, data, len);
					break;

				case NalTypePaci:
					MS_WARN_DEV("ignoring unsupported H265 PACI packet");
					return nullptr;

				default:
					parsed = ParseSingleNalUnit(*payloadDescriptor, data);
					break;
			}

			if (!parsed)
			{
				return nullptr;
			}

			return payloadDescriptor.release();
		}

		void H265::ProcessRtpPacket(RTC::RtpPacket* packet)
		{
			MS_TRACE();

			auto* data = packet->GetPayload();
			auto len   = packet->GetPayloadLength();
			RtpPacket::FrameMarking* frameMarking{ nullptr };
			uint8_t frameMarkingLen{ 0 };

			packet->ReadFrameMarking(&frameMarking, frameMarkingLen);

			auto payloadDescriptor =
			  std::unique_ptr<PayloadDescriptor>(H265::Parse(data, len, frameMarking, frameMarkingLen));

			if (!payloadDescriptor)
			{
				return;
			}

			auto payloadDescriptorHandler =
			  std::make_shared<PayloadDescriptorHandler>(payloadDescriptor.get());
			payloadDescriptor.release();

			packet->SetPayloadDescriptorHandler(std::move(payloadDescriptorHandler));
		}

		/* Instance methods. */

		void H265::PayloadDescriptor::Dump() const
		{
			MS_TRACE();

			MS_DUMP("<H265::PayloadDescriptor>");
			MS_DUMP(
			  "  s:%" PRIu8 "|e:%" PRIu8 "|i:%" PRIu8 "|d:%" PRIu8 "|b:%" PRIu8,
			  this->s,
			  this->e,
			  this->i,
			  this->d,
			  this->b);
			if (this->hasTid)
			{
				MS_DUMP("  tid: %" PRIu8, this->tid);
			}
			if (this->hasLid)
			{
				MS_DUMP("  lid: %" PRIu8, this->lid);
			}
			if (this->hasTl0picidx)
			{
				MS_DUMP("  tl0picidx: %" PRIu8, this->tl0picidx);
			}
			MS_DUMP("  isKeyFrame: %s", this->isKeyFrame ? "true" : "false");
			MS_DUMP(
			  "  parameterSets: vps=%s sps=%s pps=%s",
			  this->hasVps ? "true" : "false",
			  this->hasSps ? "true" : "false",
			  this->hasPps ? "true" : "false");
			MS_DUMP("</H265::PayloadDescriptor>");
		}

		H265::PayloadDescriptorHandler::PayloadDescriptorHandler(H265::PayloadDescriptor* payloadDescriptor)
		{
			MS_TRACE();

			this->payloadDescriptor.reset(payloadDescriptor);
		}

		bool H265::PayloadDescriptorHandler::Process(
		  RTC::Codecs::EncodingContext* encodingContext, uint8_t* /*data*/, bool& /*marker*/)
		{
			MS_TRACE();

			auto* context = static_cast<RTC::Codecs::H265::EncodingContext*>(encodingContext);

			MS_ASSERT(context->GetTargetTemporalLayer() >= 0, "target temporal layer cannot be -1");

			if (context->GetTemporalLayers() > 1 && !this->payloadDescriptor->hasTid)
			{
				MS_WARN_DEV("stream is supposed to have >1 temporal layers but does not have tid field");
			}

			if (this->payloadDescriptor->hasTid && this->payloadDescriptor->tid > context->GetTargetTemporalLayer())
			{
				return false;
			}
			else if (
			  this->payloadDescriptor->hasTid &&
			  this->payloadDescriptor->tid > context->GetCurrentTemporalLayer() &&
			  !this->payloadDescriptor->b)
			{
				return false;
			}

			if (this->payloadDescriptor->hasTid && this->payloadDescriptor->tid > context->GetCurrentTemporalLayer())
			{
				context->SetCurrentTemporalLayer(this->payloadDescriptor->tid);
			}
			else if (!this->payloadDescriptor->hasTid)
			{
				context->SetCurrentTemporalLayer(0);
			}

			if (context->GetCurrentTemporalLayer() > context->GetTargetTemporalLayer())
			{
				context->SetCurrentTemporalLayer(context->GetTargetTemporalLayer());
			}

			return true;
		}

		void H265::PayloadDescriptorHandler::Restore(uint8_t* /*data*/) noexcept
		{
			MS_TRACE();
		}
	} // namespace Codecs
} // namespace RTC

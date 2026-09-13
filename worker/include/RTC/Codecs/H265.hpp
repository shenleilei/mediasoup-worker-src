#ifndef MS_RTC_CODECS_H265_HPP
#define MS_RTC_CODECS_H265_HPP

#include "common.hpp"
#include "RTC/Codecs/PayloadDescriptorHandler.hpp"
#include "RTC/RtpPacket.hpp"

namespace RTC
{
	namespace Codecs
	{
		class H265
		{
		public:
			struct PayloadDescriptor : public RTC::Codecs::PayloadDescriptor
			{
				~PayloadDescriptor() = default;

				void Dump() const override;

				uint8_t s{ 0 };   // Start of Frame.
				uint8_t e{ 0 };   // End of Frame.
				uint8_t i{ 0 };   // Independent Frame.
				uint8_t d{ 0 };   // Discardable Frame.
				uint8_t b{ 0 };   // Base Layer Sync.
				uint8_t tid{ 0 }; // Temporal layer id.
				uint8_t lid{ 0 }; // Spatial layer id.
				uint8_t tl0picidx{ 0 };

				bool hasLid{ false };
				bool hasTid{ false };
				bool hasTl0picidx{ false };
				bool isKeyFrame{ false };
				// NAL-level IRAP traffic: true on ANY fragment of an IRAP NAL
				// (the FU header repeats the original NAL type on every
				// fragment), so key-frame traffic remains observable even when
				// the FU start fragment itself is lost.
				bool isKeyFrameNal{ false };
				bool hasVps{ false };
				bool hasSps{ false };
				bool hasPps{ false };
				bool hasFrameMarking{ false };
				bool isFragmentStart{ false };
				bool isFragmentEnd{ false };
				// first_slice_segment_in_pic_flag read from the slice segment
				// header.  Authoritative picture-start signal per H.265
				// 7.3.6.1: exactly one slice segment of a picture has it set,
				// regardless of the number of slices.
				bool isFirstSliceOfPicture{ false };
				// True when the slice segment header was present and readable
				// in this packet (single NAL, FU start fragment, or an
				// aggregation sub-NAL).  A false flag read from a readable
				// header is credible evidence that this packet is NOT the
				// picture start; a missing header is unknown, not false.
				bool isFirstSliceCredible{ false };
			};

		public:
			static H265::PayloadDescriptor* Parse(
			  const uint8_t* data,
			  size_t len,
			  RTC::RtpPacket::FrameMarking* frameMarking = nullptr,
			  uint8_t frameMarkingLen                    = 0);
			static void ProcessRtpPacket(RTC::RtpPacket* packet);

		public:
			class EncodingContext : public RTC::Codecs::EncodingContext
			{
			public:
				explicit EncodingContext(RTC::Codecs::EncodingContext::Params& params)
				  : RTC::Codecs::EncodingContext(params)
				{
				}
				~EncodingContext() = default;

			public:
				void SyncRequired() override
				{
				}
			};

		public:
			class PayloadDescriptorHandler : public RTC::Codecs::PayloadDescriptorHandler
			{
			public:
				explicit PayloadDescriptorHandler(PayloadDescriptor* payloadDescriptor);
				~PayloadDescriptorHandler() = default;

			public:
				void Dump() const override
				{
					this->payloadDescriptor->Dump();
				}
				bool Process(RTC::Codecs::EncodingContext* encodingContext, uint8_t* data, bool& marker) override;
				void Restore(uint8_t* data) noexcept override;
				uint8_t GetSpatialLayer() const override
				{
					return 0u;
				}
				uint8_t GetTemporalLayer() const override
				{
					return this->payloadDescriptor->tid;
				}
				bool IsKeyFrame() const override
				{
					return this->payloadDescriptor->isKeyFrame;
				}
				bool IsKeyFrameNal() const override
				{
					return this->payloadDescriptor->isKeyFrameNal;
				}
				bool IsFrameStart() const override
				{
					// Frame marking is authoritative when negotiated; do not mix it
					// with slice-header evidence on the same stream.
					if (this->payloadDescriptor->hasFrameMarking)
					{
						return this->payloadDescriptor->s != 0;
					}
					// first_slice_segment_in_pic_flag is an authoritative
					// picture-start signal: a later slice of the same picture can
					// never set it.  A missing/unreadable slice header is unknown.
					return this->payloadDescriptor->isFirstSliceCredible &&
					       this->payloadDescriptor->isFirstSliceOfPicture;
				}
				bool IsFrameStartFromSliceHeader() const override
				{
					return !this->payloadDescriptor->hasFrameMarking;
				}
				bool IsFrameEnd(bool rtpMarker) const override
				{
					if (this->payloadDescriptor->hasFrameMarking)
					{
						return this->payloadDescriptor->e != 0;
					}
					// The RTP marker bit marks the last packet of an access unit
					// for H.265 video (RFC 7798 4.4).  It covers FU, single NAL
					// and aggregation packets alike; FU fragment-end alone only
					// marks a NAL boundary.
					return rtpMarker;
				}

			private:
				std::unique_ptr<PayloadDescriptor> payloadDescriptor;
			};
		};
	} // namespace Codecs
} // namespace RTC

#endif

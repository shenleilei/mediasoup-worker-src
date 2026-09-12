#ifndef MS_RTC_CODECS_H264_HPP
#define MS_RTC_CODECS_H264_HPP

#include "common.hpp"
#include "RTC/Codecs/PayloadDescriptorHandler.hpp"
#include "RTC/RtpPacket.hpp"

namespace RTC
{
	namespace Codecs
	{
		class H264
		{
		public:
			struct PayloadDescriptor : public RTC::Codecs::PayloadDescriptor
			{
				/* Pure virtual methods inherited from RTC::Codecs::PayloadDescriptor. */
				~PayloadDescriptor() = default;

				void Dump() const override;

				// Fields in frame-marking extension.
				uint8_t s : 1;          // Start of Frame.
				uint8_t e : 1;          // End of Frame.
				uint8_t i : 1;          // Independent Frame.
				uint8_t d : 1;          // Discardable Frame.
				uint8_t b : 1;          // Base Layer Sync.
				uint8_t tid{ 0 };       // Temporal layer id.
				uint8_t lid{ 0 };       // Spatial layer id.
				uint8_t tl0picidx{ 0 }; // TL0PICIDX

				// Parsed values.
				bool hasLid{ false };
				bool hasTid{ false };
				bool hasTl0picidx{ false };
				bool isKeyFrame{ false };
				bool hasFrameMarking{ false };
				bool isFragmentStart{ false };
				bool isFragmentEnd{ false };
				// first_mb_in_slice == 0 read from the slice header.  Unlike the
				// H.265 first_slice_segment_in_pic_flag this is a restricted
				// heuristic: it identifies the first slice in decoding order only
				// for streams that do not use FMO (multiple slice groups) or
				// arbitrary slice order.  Producer keeps a runtime violation
				// detector (two first-slice markers within one picture) and
				// disables the heuristic per SSRC when it fires.
				bool isFirstSliceOfPicture{ false };
				// True when the slice header was present and readable in this
				// packet.  A false flag from a readable header is credible
				// evidence that this packet is NOT the first slice; a missing
				// header is unknown, not false.
				bool isFirstSliceCredible{ false };
			};

		public:
			static H264::PayloadDescriptor* Parse(
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

				/* Pure virtual methods inherited from RTC::Codecs::EncodingContext. */
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
				bool IsFrameStart() const override
				{
					// Frame marking is authoritative when negotiated; do not mix it
					// with slice-header evidence on the same stream.
					if (this->payloadDescriptor->hasFrameMarking)
					{
						return this->payloadDescriptor->s != 0;
					}
					// Restricted heuristic: first_mb_in_slice == 0 identifies the
					// first slice of a picture only without FMO/ASO (see
					// PayloadDescriptor).  A later slice can never carry
					// first_mb == 0 in that restricted configuration.
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
					// for H.264 video (RFC 6184 5.1).  It covers FU, single NAL
					// and STAP packets alike; a FU end bit alone only marks a NAL
					// boundary.
					return rtpMarker;
				}

			private:
				std::unique_ptr<PayloadDescriptor> payloadDescriptor;
			};
		};
	} // namespace Codecs
} // namespace RTC

#endif

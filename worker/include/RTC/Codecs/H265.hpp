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
				bool hasVps{ false };
				bool hasSps{ false };
				bool hasPps{ false };
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
				void Restore(uint8_t* data) override;
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

			private:
				std::unique_ptr<PayloadDescriptor> payloadDescriptor;
			};
		};
	} // namespace Codecs
} // namespace RTC

#endif

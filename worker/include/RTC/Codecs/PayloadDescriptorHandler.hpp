#ifndef MS_RTC_CODECS_PAYLOAD_DESCRIPTOR_HANDLER_HPP
#define MS_RTC_CODECS_PAYLOAD_DESCRIPTOR_HANDLER_HPP

#include "common.hpp"

namespace RTC
{
	namespace Codecs
	{
		// Codec payload descriptor.
		struct PayloadDescriptor
		{
			virtual ~PayloadDescriptor() = default;
			virtual void Dump() const    = 0;
		};

		// Encoding context used by PayloadDescriptorHandler to properly rewrite the
		// PayloadDescriptor.
		class EncodingContext
		{
		public:
			struct Params
			{
				uint8_t spatialLayers{ 1u };
				uint8_t temporalLayers{ 1u };
				bool ksvc{ false };
			};

		public:
			explicit EncodingContext(RTC::Codecs::EncodingContext::Params& params) : params(params)
			{
			}
			virtual ~EncodingContext() = default;

		public:
			uint8_t GetSpatialLayers() const
			{
				return this->params.spatialLayers;
			}
			uint8_t GetTemporalLayers() const
			{
				return this->params.temporalLayers;
			}
			bool IsKSvc() const
			{
				return this->params.ksvc;
			}
			int16_t GetTargetSpatialLayer() const
			{
				return this->targetSpatialLayer;
			}
			int16_t GetTargetTemporalLayer() const
			{
				return this->targetTemporalLayer;
			}
			int16_t GetCurrentSpatialLayer() const
			{
				return this->currentSpatialLayer;
			}
			int16_t GetCurrentTemporalLayer() const
			{
				return this->currentTemporalLayer;
			}
			bool GetIgnoreDtx() const
			{
				return this->ignoreDtx;
			}
			void SetTargetSpatialLayer(int16_t spatialLayer)
			{
				this->targetSpatialLayer = spatialLayer;
			}
			void SetTargetTemporalLayer(int16_t temporalLayer)
			{
				this->targetTemporalLayer = temporalLayer;
			}
			void SetCurrentSpatialLayer(int16_t spatialLayer)
			{
				this->currentSpatialLayer = spatialLayer;
			}
			void SetCurrentTemporalLayer(int16_t temporalLayer)
			{
				this->currentTemporalLayer = temporalLayer;
			}
			void SetIgnoreDtx(bool ignoreDtx)
			{
				this->ignoreDtx = ignoreDtx;
			}
			virtual void SyncRequired() = 0;

		private:
			Params params;
			int16_t targetSpatialLayer{ -1 };
			int16_t targetTemporalLayer{ -1 };
			int16_t currentSpatialLayer{ -1 };
			int16_t currentTemporalLayer{ -1 };
			bool ignoreDtx{ false };
		};

		class PayloadDescriptorHandler
		{
		public:
			virtual ~PayloadDescriptorHandler() = default;

		public:
			virtual void Dump() const                                                                = 0;
			virtual bool Process(RTC::Codecs::EncodingContext* context, uint8_t* data, bool& marker) = 0;
			virtual void Restore(uint8_t* data) noexcept                                             = 0;
			virtual uint8_t GetSpatialLayer() const                                                  = 0;
			virtual uint8_t GetTemporalLayer() const                                                 = 0;
			virtual bool IsKeyFrame() const                                                          = 0;
			// Codec-parsed frame boundary information.  A key-frame candidate may
			// only start at IsFrameStart(); IsFrameEnd() must be corroborated by
			// the RTP marker when the codec itself does not provide a reliable
			// frame-end bit.
			virtual bool IsFrameStart() const
			{
				return false;
			}
			virtual bool IsFrameEnd(bool /*rtpMarker*/) const
			{
				return false;
			}
			// True when IsFrameStart() was derived from a slice header
			// (H.264 first_mb_in_slice / H.265 first_slice_segment_in_pic_flag)
			// instead of an RTP extension or codec descriptor.  Only
			// slice-header starts carry the per-picture uniqueness contract
			// used by Producer's frame-start conflict detector; e.g. VP9's
			// layer-start bit legitimately repeats within one picture across
			// spatial layers.
			virtual bool IsFrameStartFromSliceHeader() const
			{
				return false;
			}
		};
	} // namespace Codecs
} // namespace RTC

#endif

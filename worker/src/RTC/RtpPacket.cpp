#define MS_CLASS "RTC::RtpPacket"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/RtpPacket.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include <algorithm>
#include <cstring>  // std::memcpy(), std::memmove(), std::memset()
#include <iterator> // std::ostream_iterator
#include <limits>
#include <memory>
#include <sstream> // std::ostringstream

namespace RTC
{
	/* Class methods. */

	RtpPacket* RtpPacket::Parse(const uint8_t* data, size_t len, size_t capacity)
	{
		MS_TRACE();

		if (capacity < len)
		{
			return nullptr;
		}

		if (!RtpPacket::IsRtp(data, len))
		{
			return nullptr;
		}

		auto* ptr = const_cast<uint8_t*>(data);

		// Get the header.
		auto* header = reinterpret_cast<Header*>(ptr);

		// Inspect data after the minimum header size.
		ptr += HeaderSize;

		// Check CSRC list.
		size_t csrcListSize{ 0u };

		if (header->csrcCount != 0u)
		{
			csrcListSize = header->csrcCount * sizeof(header->ssrc);

			// Packet size must be >= header size + CSRC list.
			if (len < (ptr - data) + csrcListSize)
			{
				MS_WARN_TAG(rtp, "not enough space for the announced CSRC list, packet discarded");

				return nullptr;
			}
			ptr += csrcListSize;
		}

		// Check header extension.
		HeaderExtension* headerExtension{ nullptr };
		size_t extensionValueSize{ 0u };

		if (header->extension == 1u)
		{
			// The header extension is at least 4 bytes.
			if (len < static_cast<size_t>(ptr - data) + 4)
			{
				MS_WARN_TAG(rtp, "not enough space for the announced header extension, packet discarded");

				return nullptr;
			}

			headerExtension = reinterpret_cast<HeaderExtension*>(ptr);

			// The header extension contains a 16-bit length field that counts the number of
			// 32-bit words in the extension, excluding the four-octet header extension.
			extensionValueSize = static_cast<size_t>(ntohs(headerExtension->length) * 4);

			// Packet size must be >= header size + CSRC list + header extension size.
			if (len < (ptr - data) + 4 + extensionValueSize)
			{
				MS_WARN_TAG(
				  rtp, "not enough space for the announced header extension value, packet discarded");

				return nullptr;
			}
			ptr += 4 + extensionValueSize;
		}

		// Get payload.
		uint8_t* payload     = ptr;
		size_t payloadLength = len - (ptr - data);
		uint8_t payloadPadding{ 0 };

		MS_ASSERT(len >= static_cast<size_t>(ptr - data), "payload has negative size");

		// Check padding field.
		if (header->padding != 0u)
		{
			// Must be at least a single payload byte.
			if (payloadLength == 0)
			{
				MS_WARN_TAG(rtp, "padding bit is set but no space for a padding byte, packet discarded");

				return nullptr;
			}

			payloadPadding = data[len - 1];

			if (payloadPadding == 0)
			{
				MS_WARN_TAG(rtp, "padding byte cannot be 0, packet discarded");

				return nullptr;
			}

			if (payloadLength < size_t{ payloadPadding })
			{
				MS_WARN_TAG(
				  rtp,
				  "number of padding octets is greater than available space for payload, packet "
				  "discarded");

				return nullptr;
			}

			payloadLength -= size_t{ payloadPadding };
		}

		MS_ASSERT(
		  len == HeaderSize + csrcListSize + (headerExtension ? 4 + extensionValueSize : 0) +
		           payloadLength + size_t{ payloadPadding },
		  "packet's computed size does not match received size");

		return new RtpPacket(
		  header, headerExtension, payload, payloadLength, payloadPadding, len, capacity);
	}

	/* Instance methods. */

	RtpPacket::RtpPacket(
	  Header* header,
	  HeaderExtension* headerExtension,
	  const uint8_t* payload,
	  size_t payloadLength,
	  uint8_t payloadPadding,
	  size_t size,
	  size_t capacity)
	  : header(header), headerExtension(headerExtension), payload(const_cast<uint8_t*>(payload)),
	    payloadLength(payloadLength), payloadPadding(payloadPadding), size(size), capacity(capacity)
	{
		MS_TRACE();

		if (this->header->csrcCount != 0u)
		{
			this->csrcList = reinterpret_cast<uint8_t*>(header) + HeaderSize;
		}

		// Parse RFC 5285 header extension.
		ParseExtensions();

// Avoid retrieving the time if RTC logger is disabled.
#ifdef MS_RTC_LOGGER_RTP
		// Initialize logger.
		this->logger.timestamp        = DepLibUV::GetTimeMs();
		this->logger.recvRtpTimestamp = this->GetTimestamp();
		this->logger.recvSeqNumber    = this->GetSequenceNumber();
#endif
	}

	RtpPacket::~RtpPacket()
	{
		MS_TRACE();

		delete[] this->buffer;
	}

	void RtpPacket::Dump() const
	{
		MS_TRACE();

		MS_DUMP("<RtpPacket>");
		MS_DUMP("  padding: %s", this->header->padding ? "true" : "false");
		if (HasHeaderExtension())
		{
			MS_DUMP(
			  "  header extension: id:%" PRIu16 ", length:%zu",
			  GetHeaderExtensionId(),
			  GetHeaderExtensionLength());
		}
		if (HasOneByteExtensions())
		{
			MS_DUMP("  RFC5285 ext style: One-Byte Header");
		}
		if (HasTwoBytesExtensions())
		{
			MS_DUMP("  RFC5285 ext style: Two-Bytes Header");
		}
		if (HasOneByteExtensions() || HasTwoBytesExtensions())
		{
			std::vector<std::string> extIds;
			std::ostringstream extIdsStream;

			if (HasOneByteExtensions())
			{
				for (const auto& extension : this->oneByteExtensions)
				{
					if (extension != nullptr)
					{
						extIds.push_back(std::to_string(extension->id));
					}
				}
			}
			else
			{
				extIds.reserve(this->mapTwoBytesExtensions.size());

				for (const auto& kv : this->mapTwoBytesExtensions)
				{
					extIds.push_back(std::to_string(kv.first));
				}
			}

			if (!extIds.empty())
			{
				std::copy(
				  extIds.begin(), extIds.end() - 1, std::ostream_iterator<std::string>(extIdsStream, ","));
				extIdsStream << extIds.back();

				MS_DUMP("  RFC5285 ext ids: %s", extIdsStream.str().c_str());
			}
		}
		if (this->midExtensionId != 0u)
		{
			std::string mid;

			if (ReadMid(mid))
			{
				MS_DUMP("  mid: extId:%" PRIu8 ", value:'%s'", this->midExtensionId, mid.c_str());
			}
		}
		if (this->ridExtensionId != 0u)
		{
			std::string rid;

			if (ReadRid(rid))
			{
				MS_DUMP("  rid: extId:%" PRIu8 ", value:'%s'", this->ridExtensionId, rid.c_str());
			}
		}
		if (this->rridExtensionId != 0u)
		{
			std::string rid;

			if (ReadRid(rid))
			{
				MS_DUMP("  rrid: extId:%" PRIu8 ", value:'%s'", this->rridExtensionId, rid.c_str());
			}
		}
		if (this->absSendTimeExtensionId != 0u)
		{
			MS_DUMP("  absSendTime: extId:%" PRIu8, this->absSendTimeExtensionId);
		}
		if (this->transportWideCc01ExtensionId != 0u)
		{
			uint16_t wideSeqNumber;

			if (ReadTransportWideCc01(wideSeqNumber))
			{
				MS_DUMP(
				  "  transportWideCc01: extId:%" PRIu8 ", value:%" PRIu16,
				  this->transportWideCc01ExtensionId,
				  wideSeqNumber);
			}
		}
		// Remove once it becomes RFC.
		if (this->frameMarking07ExtensionId != 0u)
		{
			MS_DUMP("  frameMarking07: extId:%" PRIu8, this->frameMarking07ExtensionId);
		}
		if (this->frameMarkingExtensionId != 0u)
		{
			MS_DUMP("  frameMarking: extId:%" PRIu8, this->frameMarkingExtensionId);
		}
		if (this->ssrcAudioLevelExtensionId != 0u)
		{
			uint8_t volume;
			bool voice;

			if (ReadSsrcAudioLevel(volume, voice))
			{
				MS_DUMP(
				  "  ssrcAudioLevel: extId:%" PRIu8 ", volume:%" PRIu8 ", voice:%s",
				  this->ssrcAudioLevelExtensionId,
				  volume,
				  voice ? "true" : "false");
			}
		}
		if (this->videoOrientationExtensionId != 0u)
		{
			bool camera;
			bool flip;
			uint16_t rotation;

			if (ReadVideoOrientation(camera, flip, rotation))
			{
				MS_DUMP(
				  "  videoOrientation: extId:%" PRIu8 ", camera:%s, flip:%s, rotation:%" PRIu16,
				  this->videoOrientationExtensionId,
				  camera ? "true" : "false",
				  flip ? "true" : "false",
				  rotation);
			}
		}
		MS_DUMP("  csrc count: %" PRIu8, this->header->csrcCount);
		MS_DUMP("  marker: %s", HasMarker() ? "true" : "false");
		MS_DUMP("  payload type: %" PRIu8, GetPayloadType());
		MS_DUMP("  sequence number: %" PRIu16, GetSequenceNumber());
		MS_DUMP("  timestamp: %" PRIu32, GetTimestamp());
		MS_DUMP("  ssrc: %" PRIu32, GetSsrc());
		MS_DUMP("  payload size: %zu bytes", GetPayloadLength());
		if (this->header->padding != 0u)
		{
			MS_DUMP("  padding size: %" PRIu8 " bytes", this->payloadPadding);
		}
		MS_DUMP("  packet size: %zu bytes", GetSize());
		MS_DUMP("  spatial layer: %" PRIu8, GetSpatialLayer());
		MS_DUMP("  temporal layer: %" PRIu8, GetTemporalLayer());
		MS_DUMP("</RtpPacket>");
	}

	flatbuffers::Offset<FBS::RtpPacket::Dump> RtpPacket::FillBuffer(
	  flatbuffers::FlatBufferBuilder& builder) const
	{
		// Add mid.
		std::string mid;

		if (this->midExtensionId != 0u)
		{
			ReadMid(mid);
		}

		// Add rid.
		std::string rid;

		if (this->ridExtensionId != 0u)
		{
			ReadRid(rid);
		}

		// Add rrid.
		std::string rrid;

		if (this->rridExtensionId != 0u)
		{
			ReadRid(rrid);
		}

		// Add wideSequenceNumber.
		uint16_t wideSequenceNumber;
		bool wideSequenceNumberSet = false;

		if (this->transportWideCc01ExtensionId != 0u)
		{
			wideSequenceNumberSet = true;
			ReadTransportWideCc01(wideSequenceNumber);
		}

		return FBS::RtpPacket::CreateDumpDirect(
		  builder,
		  this->GetPayloadType(),
		  this->GetSequenceNumber(),
		  this->GetTimestamp(),
		  this->HasMarker(),
		  this->GetSsrc(),
		  this->IsKeyFrame(),
		  this->GetSize(),
		  this->GetPayloadLength(),
		  this->GetSpatialLayer(),
		  this->GetTemporalLayer(),
		  mid.empty() ? nullptr : mid.c_str(),
		  rid.empty() ? nullptr : rid.c_str(),
		  rrid.empty() ? nullptr : rrid.c_str(),
		  wideSequenceNumberSet ? flatbuffers::Optional<uint16_t>(wideSequenceNumber)
		                        : flatbuffers::nullopt);
	}

	bool RtpPacket::SetExtensions(uint8_t type, const std::vector<GenericExtension>& extensions)
	{
		if (type != 1u && type != 2u)
		{
			return false;
		}

		// Calculate the complete new extension area before changing packet state.
		// In particular, do not reset the extension maps or move payload bytes until
		// the backing capacity and all integer conversions have been checked.
		size_t encodedExtensionsSize{ 0 };
		size_t twoByteExtensionCount{ 0 };
		for (const auto& extension : extensions)
		{
			size_t elementSize{ 0 };
			if (type == 1u)
			{
				if (extension.id == 0 || extension.id > 14 || extension.len == 0 || extension.len > 16)
				{
					continue;
				}
				elementSize = 1u + extension.len;
			}
			else
			{
				if (extension.id == 0)
				{
					continue;
				}
				elementSize = 2u + extension.len;
				++twoByteExtensionCount;
			}

			if (extension.len != 0 && extension.value == nullptr)
			{
				return false;
			}
			if (encodedExtensionsSize > std::numeric_limits<size_t>::max() - elementSize)
			{
				return false;
			}
			encodedExtensionsSize += elementSize;
		}

		if (encodedExtensionsSize > std::numeric_limits<size_t>::max() - 3u)
		{
			return false;
		}
		const size_t paddedExtensionsTotalSize = (encodedExtensionsSize + 3u) & ~size_t{ 3u };
		const size_t padding                   = paddedExtensionsTotalSize - encodedExtensionsSize;
		if ((paddedExtensionsTotalSize / 4u) > std::numeric_limits<uint16_t>::max())
		{
			return false;
		}

		const size_t oldExtensionSize = this->headerExtension ? GetHeaderExtensionLength() : 0u;
		int64_t shift{ 0 };
		if (this->headerExtension)
		{
			if (paddedExtensionsTotalSize >= oldExtensionSize)
			{
				const size_t delta = paddedExtensionsTotalSize - oldExtensionSize;
				if (delta > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
				{
					return false;
				}
				shift = static_cast<int64_t>(delta);
			}
			else
			{
				const size_t delta = oldExtensionSize - paddedExtensionsTotalSize;
				if (delta > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
				{
					return false;
				}
				shift = -static_cast<int64_t>(delta);
			}
		}
		else
		{
			if (paddedExtensionsTotalSize > static_cast<size_t>(std::numeric_limits<int64_t>::max() - 4))
			{
				return false;
			}
			shift = 4 + static_cast<int64_t>(paddedExtensionsTotalSize);
		}

		size_t finalSize{ this->size };
		if (shift >= 0)
		{
			const auto growth = static_cast<size_t>(shift);
			if (finalSize > std::numeric_limits<size_t>::max() - growth)
			{
				return false;
			}
			finalSize += growth;
		}
		else
		{
			const auto shrink = static_cast<size_t>(-shift);
			if (finalSize < shrink)
			{
				return false;
			}
			finalSize -= shrink;
		}
		if (finalSize > this->capacity)
		{
			return false;
		}

		absl::flat_hash_map<uint8_t, TwoBytesExtension*> preparedTwoBytesExtensions;
		if (type == 2u)
		{
			try
			{
				// Build the complete index before mutating packet bytes. Filling its
				// prepared pointer slots and swapping it into place are allocation-free.
				preparedTwoBytesExtensions.reserve(twoByteExtensionCount);
				for (const auto& extension : extensions)
				{
					if (extension.id != 0u)
					{
						preparedTwoBytesExtensions.emplace(extension.id, nullptr);
					}
				}
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
		}

		// Reset extension ids.
		this->midExtensionId               = 0u;
		this->ridExtensionId               = 0u;
		this->rridExtensionId              = 0u;
		this->absSendTimeExtensionId       = 0u;
		this->absCaptureTimeExtensionId    = 0u;
		this->transportWideCc01ExtensionId = 0u;
		this->frameMarking07ExtensionId    = 0u;
		this->frameMarkingExtensionId      = 0u;
		this->ssrcAudioLevelExtensionId    = 0u;
		this->videoOrientationExtensionId  = 0u;

		// Clear the One-Byte extension index. The Two-Bytes index remains intact
		// until its prepared replacement is committed at the end.
		std::fill(std::begin(this->oneByteExtensions), std::end(this->oneByteExtensions), nullptr);
		if (type == 1u)
		{
			this->mapTwoBytesExtensions.clear();
		}

		const size_t extensionsTotalSize = paddedExtensionsTotalSize;

		// If the existing header extension uses the other format, update its id only
		// after the preflight above has proved that the packet can be rewritten.
		if (this->headerExtension)
		{
			if (type == 1u && !HasOneByteExtensions())
			{
				this->headerExtension->id = uint16_t{ htons(0xBEDE) };
			}
			else if (type == 2u && !HasTwoBytesExtensions())
			{
				this->headerExtension->id = uint16_t{ htons(0b0001000000000000) };
			}
		}

		if (this->headerExtension && shift != 0)
		{
			// Shift the payload.
			std::memmove(this->payload + shift, this->payload, this->payloadLength + this->payloadPadding);
			this->payload += shift;

			// Update packet total size.
			this->size = finalSize;

			// Update the header extension length.
			this->headerExtension->length = htons(extensionsTotalSize / 4);
		}
		else if (!this->headerExtension)
		{
			// Set the header extension bit.
			this->header->extension = 1u;

			// Set the header extension pointing to the current payload.
			this->headerExtension = reinterpret_cast<HeaderExtension*>(this->payload);

			// Shift the payload.
			std::memmove(this->payload + shift, this->payload, this->payloadLength + this->payloadPadding);
			this->payload += shift;

			// Update packet total size.
			this->size = finalSize;

			// Set the header extension id.
			if (type == 1u)
			{
				this->headerExtension->id = uint16_t{ htons(0xBEDE) };
			}
			else if (type == 2u)
			{
				this->headerExtension->id = uint16_t{ htons(0b0001000000000000) };
			}

			// Set the header extension length.
			this->headerExtension->length = htons(extensionsTotalSize / 4);
		}

		// Write the new extensions into the header extension value.
		uint8_t* ptr = this->headerExtension->value;

		for (const auto& extension : extensions)
		{
			if (type == 1u)
			{
				if (extension.id == 0 || extension.id > 14 || extension.len == 0 || extension.len > 16)
				{
					continue;
				}

				// Store the One-Byte extension element in an array.
				// `-1` because we have 14 elements total 0..13 and `id` is in the range 1..14.
				this->oneByteExtensions[extension.id - 1] = reinterpret_cast<OneByteExtension*>(ptr);

				*ptr = (extension.id << 4) | ((extension.len - 1) & 0x0F);
				++ptr;
				if (extension.len != 0u)
				{
					std::memmove(ptr, extension.value, extension.len);
				}
				ptr += extension.len;
			}
			else if (type == 2u)
			{
				if (extension.id == 0)
				{
					continue;
				}

				// Fill the already allocated Two-Bytes extension index entry.
				preparedTwoBytesExtensions.find(extension.id)->second =
				  reinterpret_cast<TwoBytesExtension*>(ptr);

				*ptr = extension.id;
				++ptr;
				*ptr = extension.len;
				++ptr;
				if (extension.len != 0u)
				{
					std::memmove(ptr, extension.value, extension.len);
				}
				ptr += extension.len;
			}
		}

		for (size_t i = 0; i < padding; ++i)
		{
			*ptr = 0u;
			++ptr;
		}

		MS_ASSERT(ptr == this->payload, "wrong ptr calculation");
		if (type == 2u)
		{
			this->mapTwoBytesExtensions.swap(preparedTwoBytesExtensions);
		}
		return true;
	}

	bool RtpPacket::UpdateMid(const std::string& mid)
	{
		MS_TRACE();

		uint8_t extenLen;
		uint8_t* extenValue = GetExtension(this->midExtensionId, extenLen);

		if (!extenValue)
		{
			return false;
		}

		const size_t midLen = mid.length();
		if (midLen == 0u || midLen > std::numeric_limits<uint8_t>::max())
		{
			return false;
		}

		// Resize first. SetExtensionLength validates that any growth only consumes
		// padding owned by this element, so a failure leaves packet bytes untouched.
		if (!SetExtensionLength(this->midExtensionId, static_cast<uint8_t>(midLen)))
		{
			return false;
		}

		std::memcpy(extenValue, mid.data(), midLen);

		return true;
	}

	bool RtpPacket::SetExtensionLength(uint8_t id, uint8_t len)
	{
		MS_TRACE();

		if (len == 0u)
		{
			MS_ERROR("cannot set extension length to 0");

			return false;
		}

		if (id == 0u)
		{
			return false;
		}
		else if (HasOneByteExtensions())
		{
			if (id > 14u || len > 16u)
			{
				return false;
			}

			// `-1` because we have 14 elements total 0..13 and `id` is in the range 1..14.
			auto* extension = this->oneByteExtensions[id - 1];

			if (!extension)
			{
				return false;
			}

			const size_t currentLen = extension->len + 1u;

			if (len > currentLen)
			{
				const auto* extensionEnd =
				  reinterpret_cast<const uint8_t*>(this->headerExtension) + 4u + GetHeaderExtensionLength();
				const uint8_t* ptr = extension->value + currentLen;
				size_t availableLength{ currentLen };

				while (availableLength < len && ptr < extensionEnd && *ptr == 0u)
				{
					++availableLength;
					++ptr;
				}

				if (availableLength < len)
				{
					return false;
				}
			}

			// Fill with 0's if new length is minor.
			if (len < currentLen)
			{
				std::memset(extension->value + len, 0, currentLen - len);
			}

			// In One-Byte extensions value length 0 means 1.
			extension->len = len - 1;

			return true;
		}
		else if (HasTwoBytesExtensions())
		{
			auto it = this->mapTwoBytesExtensions.find(id);

			if (it == this->mapTwoBytesExtensions.end())
			{
				return false;
			}

			auto* extension         = it->second;
			const size_t currentLen = extension->len;

			if (len > currentLen)
			{
				const auto* extensionEnd =
				  reinterpret_cast<const uint8_t*>(this->headerExtension) + 4u + GetHeaderExtensionLength();
				const uint8_t* ptr = extension->value + currentLen;
				size_t availableLength{ currentLen };

				while (availableLength < len && ptr < extensionEnd && *ptr == 0u)
				{
					++availableLength;
					++ptr;
				}

				if (availableLength < len)
				{
					return false;
				}
			}

			// Fill with 0's if new length is minor.
			if (len < currentLen)
			{
				std::memset(extension->value + len, 0, currentLen - len);
			}

			extension->len = len;

			return true;
		}
		else
		{
			return false;
		}
	}

	/**
	 * NOTE: This method automatically removes payload padding if present.
	 */
	bool RtpPacket::SetPayloadLength(size_t length)
	{
		MS_TRACE();

		const size_t padding = static_cast<size_t>(this->payloadPadding);
		if (this->payloadLength > this->size || padding > this->size - this->payloadLength)
		{
			return false;
		}

		const size_t headerSize = this->size - this->payloadLength - padding;
		if (headerSize > this->capacity || length > this->capacity - headerSize)
		{
			return false;
		}

		// Remove padding if present.
		if (this->payloadPadding != 0u)
		{
			SetPayloadPaddingFlag(false);
			this->payloadPadding = 0u;
		}

		this->payloadLength = length;
		this->size          = headerSize + length;

		return true;
	}

	RtpPacket* RtpPacket::Clone() const
	{
		MS_TRACE();

		const size_t retransmissionCapacity =
		  this->size <= std::numeric_limits<size_t>::max() - 2u ? this->size + 2u : this->size;
		const size_t bufferCapacity = std::max(MtuSize + 100u, retransmissionCapacity);
		auto ownedBuffer            = std::make_unique<uint8_t[]>(bufferCapacity);
		auto* buffer                = ownedBuffer.get();
		auto* ptr                   = buffer;

		size_t numBytes{ 0 };

		// Copy the minimum header.
		numBytes = HeaderSize;
		std::memcpy(ptr, GetData(), numBytes);

		// Set header pointer.
		auto* newHeader = reinterpret_cast<Header*>(ptr);

		ptr += numBytes;

		// Copy CSRC list.
		if (this->csrcList != nullptr)
		{
			numBytes = this->header->csrcCount * sizeof(this->header->ssrc);
			std::memcpy(ptr, this->csrcList, numBytes);

			ptr += numBytes;
		}

		// Copy header extension.
		HeaderExtension* newHeaderExtension{ nullptr };

		if (this->headerExtension != nullptr)
		{
			numBytes = 4 + GetHeaderExtensionLength();
			std::memcpy(ptr, this->headerExtension, numBytes);

			// Set the header extension pointer.
			newHeaderExtension = reinterpret_cast<HeaderExtension*>(ptr);

			ptr += numBytes;
		}

		// Copy payload.
		uint8_t* newPayload{ ptr };

		if (this->payloadLength != 0u)
		{
			numBytes = this->payloadLength;
			std::memcpy(ptr, this->payload, numBytes);

			ptr += numBytes;
		}

		// Copy payload padding.
		if (this->payloadPadding != 0u)
		{
			const size_t padding = static_cast<size_t>(this->payloadPadding);
			std::memcpy(ptr, this->payload + this->payloadLength, padding);
			ptr += padding;
		}

		MS_ASSERT(static_cast<size_t>(ptr - buffer) == this->size, "ptr - buffer == this->size");

		// Create the new RtpPacket instance and return it.
		auto packet = std::unique_ptr<RtpPacket>(new RtpPacket(
		  newHeader,
		  newHeaderExtension,
		  newPayload,
		  this->payloadLength,
		  this->payloadPadding,
		  this->size,
		  bufferCapacity));

		// Keep already set extension ids.
		packet->midExtensionId               = this->midExtensionId;
		packet->ridExtensionId               = this->ridExtensionId;
		packet->rridExtensionId              = this->rridExtensionId;
		packet->absSendTimeExtensionId       = this->absSendTimeExtensionId;
		packet->absCaptureTimeExtensionId    = this->absCaptureTimeExtensionId;
		packet->transportWideCc01ExtensionId = this->transportWideCc01ExtensionId;
		packet->frameMarking07ExtensionId    = this->frameMarking07ExtensionId; // Remove once RFC.
		packet->frameMarkingExtensionId      = this->frameMarkingExtensionId;
		packet->ssrcAudioLevelExtensionId    = this->ssrcAudioLevelExtensionId;
		packet->videoOrientationExtensionId  = this->videoOrientationExtensionId;
		// Assign the payload descriptor handler.
		packet->payloadDescriptorHandler = this->payloadDescriptorHandler;
#ifdef MS_RTC_LOGGER_RTP
		packet->logger = this->logger;
#endif
		// Store allocated buffer.
		packet->buffer = ownedBuffer.release();

		return packet.release();
	}

	/**
	 * NOTE: This method automatically removes payload padding if present.
	 */
	bool RtpPacket::RtxEncode(uint8_t payloadType, uint32_t ssrc, uint16_t seq)
	{
		MS_TRACE();

		const size_t padding = static_cast<size_t>(this->payloadPadding);
		if (this->size < padding)
		{
			return false;
		}

		const size_t unpaddedSize = this->size - padding;
		if (unpaddedSize > this->capacity || 2u > this->capacity - unpaddedSize)
		{
			return false;
		}

		// Rewrite the payload type.
		SetPayloadType(payloadType);

		// Rewrite the SSRC.
		SetSsrc(ssrc);

		// Write the original sequence number at the begining of the payload.
		std::memmove(this->payload + 2, this->payload, this->payloadLength);
		Utils::Byte::Set2Bytes(this->payload, 0, GetSequenceNumber());

		// Rewrite the sequence number.
		SetSequenceNumber(seq);

		// Fix the payload length.
		this->payloadLength += 2u;

		// Fix the packet size.
		this->size = unpaddedSize + 2u;

		// Remove padding if present.
		if (this->payloadPadding != 0u)
		{
			SetPayloadPaddingFlag(false);

			this->payloadPadding = 0u;
		}

		return true;
	}

	/**
	 * NOTE: This method automatically removes payload padding if present.
	 */
	bool RtpPacket::RtxDecode(uint8_t payloadType, uint32_t ssrc)
	{
		MS_TRACE();

		// Chrome sends some RTX packets with no payload when the stream is started.
		// Just ignore them.
		if (this->payloadLength < 2u)
		{
			return false;
		}

		// Rewrite the payload type.
		SetPayloadType(payloadType);

		// Rewrite the sequence number.
		SetSequenceNumber(Utils::Byte::Get2Bytes(this->payload, 0));

		// Rewrite the SSRC.
		SetSsrc(ssrc);

		// Shift the payload to its original place.
		std::memmove(this->payload, this->payload + 2, this->payloadLength - 2);

		// Fix the payload length.
		this->payloadLength -= 2u;

		// Fix the packet size.
		this->size -= 2u;

		// Remove padding if present.
		if (this->payloadPadding != 0u)
		{
			SetPayloadPaddingFlag(false);

			this->size -= size_t{ this->payloadPadding };
			this->payloadPadding = 0u;
		}

		return true;
	}

	bool RtpPacket::ProcessPayload(RTC::Codecs::EncodingContext* context, bool& marker)
	{
		MS_TRACE();

		if (!this->payloadDescriptorHandler)
		{
			return true;
		}

		return this->payloadDescriptorHandler->Process(context, this->payload, marker);
	}

	void RtpPacket::RestorePayload() noexcept
	{
		MS_TRACE();

		if (!this->payloadDescriptorHandler)
		{
			return;
		}

		this->payloadDescriptorHandler->Restore(this->payload);
	}

	/**
	 * Shifts the payload given offset (to right or to left).
	 *
	 * NOTE: This method automatically removes payload padding if present.
	 */
	bool RtpPacket::ShiftPayload(size_t payloadOffset, size_t shift, bool expand)
	{
		MS_TRACE();

		if (shift == 0u)
		{
			return true;
		}

		if (payloadOffset >= this->payloadLength)
		{
			return false;
		}

		if (!expand && shift > this->payloadLength - payloadOffset)
		{
			return false;
		}

		const size_t padding = static_cast<size_t>(this->payloadPadding);
		if (this->size < padding)
		{
			return false;
		}

		const size_t unpaddedSize = this->size - padding;
		if (expand && (unpaddedSize > this->capacity || shift > this->capacity - unpaddedSize))
		{
			return false;
		}

		uint8_t* payloadOffsetPtr = this->payload + payloadOffset;
		size_t shiftedLen{ 0 };

		if (expand)
		{
			shiftedLen = this->payloadLength - payloadOffset;

			std::memmove(payloadOffsetPtr + shift, payloadOffsetPtr, shiftedLen);

			this->payloadLength += shift;
			this->size = unpaddedSize + shift;
		}
		else
		{
			shiftedLen = this->payloadLength - payloadOffset - shift;

			std::memmove(payloadOffsetPtr, payloadOffsetPtr + shift, shiftedLen);

			this->payloadLength -= shift;
			this->size = unpaddedSize - shift;
		}

		// Remove padding if present.
		if (this->payloadPadding != 0u)
		{
			SetPayloadPaddingFlag(false);

			this->payloadPadding = 0u;
		}

		return true;
	}

	void RtpPacket::ParseExtensions()
	{
		MS_TRACE();

		// Parse One-Byte header extension.
		if (HasOneByteExtensions())
		{
			// Clear the One-Byte extension elements map.
			std::fill(std::begin(this->oneByteExtensions), std::end(this->oneByteExtensions), nullptr);

			uint8_t* extensionStart = reinterpret_cast<uint8_t*>(this->headerExtension) + 4;
			uint8_t* extensionEnd   = extensionStart + GetHeaderExtensionLength();
			uint8_t* ptr            = extensionStart;

			// One-Byte extensions cannot have length 0.
			while (ptr < extensionEnd)
			{
				const uint8_t id = (*ptr & 0xF0) >> 4;
				const size_t len = static_cast<size_t>(*ptr & 0x0F) + 1;

				// id=15 in One-Byte extensions means "stop parsing here".
				if (id == 15u)
				{
					break;
				}

				// Valid extension id.
				if (id != 0u)
				{
					if (ptr + 1 + len > extensionEnd)
					{
						MS_WARN_TAG(
						  rtp, "not enough space for the announced One-Byte header extension element value");

						break;
					}

					// Store the One-Byte extension element in an array.
					// `-1` because we have 14 elements total 0..13 and `id` is in the range 1..14.
					this->oneByteExtensions[id - 1] = reinterpret_cast<OneByteExtension*>(ptr);

					ptr += (1 + len);
				}
				// id=0 means alignment.
				else
				{
					++ptr;
				}

				// Counting padding bytes.
				while ((ptr < extensionEnd) && (*ptr == 0))
				{
					++ptr;
				}
			}
		}
		// Parse Two-Bytes header extension.
		else if (HasTwoBytesExtensions())
		{
			// Clear the Two-Bytes extension elements map.
			this->mapTwoBytesExtensions.clear();

			uint8_t* extensionStart = reinterpret_cast<uint8_t*>(this->headerExtension) + 4;
			uint8_t* extensionEnd   = extensionStart + GetHeaderExtensionLength();
			uint8_t* ptr            = extensionStart;

			// ptr points to the ID field (1 byte).
			// ptr+1 points to the length field (1 byte, can have value 0).

			// Two-Byte extensions can have length 0.
			while (ptr + 1 < extensionEnd)
			{
				const uint8_t id  = *ptr;
				const uint8_t len = *(ptr + 1);

				// Valid extension id.
				if (id != 0u)
				{
					if (ptr + 2 + len > extensionEnd)
					{
						MS_WARN_TAG(
						  rtp, "not enough space for the announced Two-Bytes header extension element value");

						break;
					}

					// Store the Two-Bytes extension element in the map.
					this->mapTwoBytesExtensions[id] = reinterpret_cast<TwoBytesExtension*>(ptr);

					ptr += (2 + len);
				}
				// id=0 means alignment.
				else
				{
					++ptr;
				}

				// Counting padding bytes.
				while ((ptr < extensionEnd) && (*ptr == 0))
				{
					++ptr;
				}
			}
		}
	}
} // namespace RTC

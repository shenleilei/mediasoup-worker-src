#define MS_CLASS "RTC::ProbeEgressAdapter"

#include "RTC/ProbeEgressAdapter.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "RTC/Consumer.hpp"
#include "RTC/RtpPacket.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace RTC
{
	namespace
	{
		uint8_t ToWireKind(RTC::Media::Kind mediaKind)
		{
			return mediaKind == RTC::Media::Kind::VIDEO ? 2u : 1u;
		}
	} // namespace

	ProbeEgressAdapter::ProbeEgressAdapter() : ProbeEgressAdapter(Config{})
	{
	}

	ProbeEgressAdapter::ProbeEgressAdapter(const ProbeEgressAdapter::Config& config) : config(config)
	{
		MS_TRACE();

		if (this->config.enabled)
		{
			this->socketReady = InitSocket();
		}
	}

	ProbeEgressAdapter::Counters ProbeEgressAdapter::GetCounters() const
	{
		return ProbeEgressAdapter::Counters{
		  this->streamMetadataByConsumerSsrc.size(),
		  this->announcedConsumerSsrcs.size(),
		  this->unknownSsrcCount,
		  this->droppedPacketCount,
		  this->exportedPacketCount,
		  this->metadataMessageCount,
		  this->packetMessageCount,
		  this->socketWriteFailureCount
		};
	}

	bool ProbeEgressAdapter::InitSocket()
	{
		if (this->config.socketPath.empty())
		{
			return false;
		}

		this->fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
		if (this->fd < 0)
		{
			MS_WARN_DEV("probe egress socket() failed: %s", std::strerror(errno));
			return false;
		}

		const int flags = ::fcntl(this->fd, F_GETFL, 0);
		if (flags >= 0)
		{
			(void)::fcntl(this->fd, F_SETFL, flags | O_NONBLOCK);
		}

		std::memset(&this->remoteAddr, 0, sizeof(this->remoteAddr));
		this->remoteAddr.sun_family = AF_UNIX;
		std::snprintf(
		  this->remoteAddr.sun_path,
		  sizeof(this->remoteAddr.sun_path),
		  "%s",
		  this->config.socketPath.c_str());

		return true;
	}

	void ProbeEgressAdapter::CloseSocket()
	{
		if (this->fd >= 0)
		{
			::close(this->fd);
			this->fd = -1;
		}
		this->socketReady = false;
	}

	bool ProbeEgressAdapter::WriteDatagram(const uint8_t* data, size_t len)
	{
		if (!data || len == 0u)
		{
			return false;
		}
		if (!this->socketReady || this->fd < 0)
		{
			++this->droppedPacketCount;
			++this->socketWriteFailureCount;
			return false;
		}

		const auto sent = ::sendto(
		  this->fd,
		  data,
		  len,
		  MSG_DONTWAIT,
		  reinterpret_cast<const sockaddr*>(&this->remoteAddr),
		  sizeof(this->remoteAddr));

		if (sent < 0)
		{
			++this->droppedPacketCount;
			++this->socketWriteFailureCount;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
			{
				MS_WARN_DEV("probe egress sendto() failed: %s", std::strerror(errno));
			}
			return false;
		}

		if (static_cast<size_t>(sent) != len)
		{
			++this->droppedPacketCount;
			++this->socketWriteFailureCount;
			return false;
		}

		return true;
	}

	void ProbeEgressAdapter::RegisterConsumerSsrc(const ProbeEgressAdapter::StreamMetadata& metadata)
	{
		if (metadata.consumerSsrc == 0u)
		{
			return;
		}

		this->streamMetadataByConsumerSsrc[metadata.consumerSsrc] = metadata;
		(void)EnsureMetadataAnnounced(metadata);
		MS_DEBUG_DEV_STD(
		  "probe metadata registered [consumerId:%s, producerId:%s, codecMime:%s, consumerSsrc:%" PRIu32 ", producerSsrc:%" PRIu32 ", isRtx:%u, pairedConsumerSsrc:%" PRIu32 ", pairedProducerSsrc:%" PRIu32 "]",
		  metadata.consumerId.c_str(),
		  metadata.producerId.c_str(),
		  metadata.codecMime.c_str(),
		  metadata.consumerSsrc,
		  metadata.producerSsrc,
		  metadata.isRtx ? 1u : 0u,
		  metadata.pairedConsumerSsrc,
		  metadata.pairedProducerSsrc);
	}

	void ProbeEgressAdapter::ExportPacket(
	  const RTC::Consumer* consumer,
	  const RTC::RtpPacket* packet,
	  ProbeEgressAdapter::PacketEventType eventType)
	{
		if (!consumer || !packet)
		{
			return;
		}

		ExportPacketData(
		  consumer->id,
		  packet->GetSsrc(),
		  packet->GetSequenceNumber(),
		  packet->GetData(),
		  packet->GetSize(),
		  eventType);
	}

	void ProbeEgressAdapter::ExportPacketData(
	  const std::string& consumerId,
	  uint32_t consumerSsrc,
	  uint16_t sequenceNumber,
	  const uint8_t* data,
	  size_t len,
	  ProbeEgressAdapter::PacketEventType eventType)
	{
		if (!this->config.enabled || !data || len == 0u)
		{
			return;
		}

		auto it = this->streamMetadataByConsumerSsrc.find(consumerSsrc);

		if (it == this->streamMetadataByConsumerSsrc.end())
		{
			++this->unknownSsrcCount;
			MS_WARN_DEV_STD(
			  "probe egress packet dropped due to unknown consumer SSRC [consumerId:%s, ssrc:%" PRIu32 "]",
			  consumerId.c_str(),
			  consumerSsrc);

			return;
		}

		if (!EnsureMetadataAnnounced(it->second))
		{
			return;
		}

		SerializePacketData(
		  consumerId,
		  consumerSsrc,
		  sequenceNumber,
		  data,
		  len,
		  eventType);
	}

	bool ProbeEgressAdapter::EnsureMetadataAnnounced(const ProbeEgressAdapter::StreamMetadata& metadata)
	{
		if (this->announcedConsumerSsrcs.find(metadata.consumerSsrc) != this->announcedConsumerSsrcs.end())
		{
			return true;
		}

		if (!SerializeMetadata(metadata))
		{
			return false;
		}

		this->announcedConsumerSsrcs.insert(metadata.consumerSsrc);
		return true;
	}

	void ProbeEgressAdapter::MaybeEmitCounters(bool force)
	{
		if (!this->config.enabled)
		{
			return;
		}

		if (!force && (this->packetMessageCount % 64u) != 0u)
		{
			return;
		}

		ProbeEgressAdapter::CountersHeader header;
		header.registeredConsumerSsrcs = static_cast<uint32_t>(this->streamMetadataByConsumerSsrc.size());
		header.announcedConsumerSsrcs  = static_cast<uint32_t>(this->announcedConsumerSsrcs.size());
		header.unknownSsrcCount        = static_cast<uint32_t>(this->unknownSsrcCount);
		header.droppedPacketCount      = static_cast<uint32_t>(this->droppedPacketCount);
		header.exportedPacketCount     = static_cast<uint32_t>(this->exportedPacketCount);
		header.metadataMessageCount    = static_cast<uint32_t>(this->metadataMessageCount);
		header.packetMessageCount      = static_cast<uint32_t>(this->packetMessageCount);
		header.socketWriteFailureCount = static_cast<uint32_t>(this->socketWriteFailureCount);

		(void)WriteDatagram(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
	}

	bool ProbeEgressAdapter::SerializeMetadata(const ProbeEgressAdapter::StreamMetadata& metadata)
	{
		if (!this->config.enabled)
		{
			return false;
		}

		ProbeEgressAdapter::MetadataHeader header;
		header.mediaKind        = ToWireKind(metadata.mediaKind);
		header.payloadType      = metadata.payloadType;
		header.consumerSsrc     = metadata.consumerSsrc;
		header.producerSsrc     = metadata.producerSsrc;
		header.clockRate        = metadata.clockRate;
		header.pairedConsumerSsrc = metadata.pairedConsumerSsrc;
		header.pairedProducerSsrc = metadata.pairedProducerSsrc;
		header.flags             = metadata.isRtx ? 0x1u : 0x0u;
		header.pairedPayloadType = metadata.pairedPayloadType;
		header.apt               = metadata.apt;
		header.consumerIdLength = static_cast<uint16_t>(metadata.consumerId.size());
		header.producerIdLength = static_cast<uint16_t>(metadata.producerId.size());
		header.codecMimeLength  = static_cast<uint16_t>(metadata.codecMime.size());

		std::vector<uint8_t> buffer;
		buffer.reserve(
		  sizeof(header) +
		  metadata.consumerId.size() +
		  metadata.producerId.size() +
		  metadata.codecMime.size());
		buffer.insert(
		  buffer.end(),
		  reinterpret_cast<const uint8_t*>(&header),
		  reinterpret_cast<const uint8_t*>(&header) + sizeof(header));
		buffer.insert(buffer.end(), metadata.consumerId.begin(), metadata.consumerId.end());
		buffer.insert(buffer.end(), metadata.producerId.begin(), metadata.producerId.end());
		buffer.insert(buffer.end(), metadata.codecMime.begin(), metadata.codecMime.end());
		if (!WriteDatagram(buffer.data(), buffer.size()))
		{
			return false;
		}

		++this->metadataMessageCount;
		MaybeEmitCounters(/*force=*/true);

		MS_DEBUG_DEV_STD(
		  "probe metadata prepared [consumerId:%s, producerId:%s, codecMime:%s, kind:%" PRIu8 ", consumerSsrc:%" PRIu32 ", producerSsrc:%" PRIu32 ", payloadType:%" PRIu8 ", clockRate:%" PRIu32 ", isRtx:%u, pairedConsumerSsrc:%" PRIu32 ", pairedProducerSsrc:%" PRIu32 ", pairedPayloadType:%" PRIu8 ", apt:%" PRIu8 "]",
		  metadata.consumerId.c_str(),
		  metadata.producerId.c_str(),
		  metadata.codecMime.c_str(),
		  ToWireKind(metadata.mediaKind),
		  metadata.consumerSsrc,
		  metadata.producerSsrc,
		  metadata.payloadType,
		  metadata.clockRate,
		  metadata.isRtx ? 1u : 0u,
		  metadata.pairedConsumerSsrc,
		  metadata.pairedProducerSsrc,
		  metadata.pairedPayloadType,
		  metadata.apt);
		return true;
	}

	void ProbeEgressAdapter::SerializePacketData(
	  const std::string& consumerId,
	  uint32_t consumerSsrc,
	  uint16_t sequenceNumber,
	  const uint8_t* data,
	  size_t len,
	  ProbeEgressAdapter::PacketEventType eventType)
	{
		if (!this->config.enabled || !data || len == 0u)
		{
			return;
		}

		if (len > this->config.maxPacketSize)
		{
			++this->droppedPacketCount;
			MS_WARN_DEV(
			  "probe egress packet exceeds configured max size [consumerId:%s, size:%zu, limit:%zu]",
			  consumerId.c_str(),
			  len,
			  this->config.maxPacketSize);

			return;
		}

		ProbeEgressAdapter::MessageHeader header;
		header.type            = ProbeEgressAdapter::MessageType::RTP_PACKET;
		header.flags           = eventType == ProbeEgressAdapter::PacketEventType::RETRANSMISSION ? 0x1u : 0u;
		header.consumerSsrc    = consumerSsrc;
		header.payloadLength   = static_cast<uint32_t>(len);
		header.monotonicTimeNs = static_cast<uint64_t>(DepLibUV::GetTimeNs());

		std::vector<uint8_t> buffer;
		buffer.reserve(sizeof(header) + len);
		buffer.insert(
		  buffer.end(),
		  reinterpret_cast<const uint8_t*>(&header),
		  reinterpret_cast<const uint8_t*>(&header) + sizeof(header));
		buffer.insert(buffer.end(), data, data + len);
		if (!WriteDatagram(buffer.data(), buffer.size()))
		{
			return;
		}

		++this->exportedPacketCount;
		++this->packetMessageCount;
		MaybeEmitCounters();

		MS_DEBUG_DEV(
		  "probe packet prepared [consumerId:%s, ssrc:%" PRIu32 ", seq:%" PRIu16 ", size:%" PRIu32 ", flags:%" PRIu8 ", tsNs:%" PRIu64 "]",
		  consumerId.c_str(),
		  header.consumerSsrc,
		  sequenceNumber,
		  header.payloadLength,
		  header.flags,
		  header.monotonicTimeNs);
	}

	ProbeEgressAdapter::~ProbeEgressAdapter()
	{
		CloseSocket();
	}
} // namespace RTC

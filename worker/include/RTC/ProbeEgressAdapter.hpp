#ifndef MS_RTC_PROBE_EGRESS_ADAPTER_HPP
#define MS_RTC_PROBE_EGRESS_ADAPTER_HPP

#include "RTC/RtpDictionaries.hpp"
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <cstddef>
#include <cstdint>
#include <sys/un.h>
#include <string>
#include <vector>

namespace RTC
{
	class Consumer;
	class RtpPacket;

	class ProbeEgressAdapter
	{
	public:
		enum class MessageType : uint8_t
		{
			STREAM_METADATA = 1,
			RTP_PACKET      = 2,
			EXPORTER_COUNTERS = 3
		};

		enum class PacketEventType : uint8_t
		{
			MEDIA          = 1,
			RETRANSMISSION = 2
		};

		struct Config
		{
			bool enabled{ false };
			std::string socketPath;
			size_t maxPacketSize{ 65536u };
		};

		struct StreamMetadata
		{
			std::string consumerId;
			std::string producerId;
			std::string codecMime;
			RTC::Media::Kind mediaKind{ RTC::Media::Kind::AUDIO };
			uint32_t consumerSsrc{ 0u };
			uint32_t producerSsrc{ 0u };
			uint8_t payloadType{ 0u };
			uint32_t clockRate{ 0u };
			bool isRtx{ false };
			uint32_t pairedConsumerSsrc{ 0u };
			uint32_t pairedProducerSsrc{ 0u };
			uint8_t pairedPayloadType{ 0u };
			uint8_t apt{ 0u };
		};

	#pragma pack(push, 1)
		struct MessageHeader
		{
			uint8_t version{ 1u };
			MessageType type{ MessageType::RTP_PACKET };
			uint8_t reserved{ 0u };
			uint8_t flags{ 0u };
			uint32_t consumerSsrc{ 0u };
			uint32_t payloadLength{ 0u };
			uint64_t monotonicTimeNs{ 0u };
		};

		struct MetadataHeader
		{
			uint8_t version{ 1u };
			MessageType type{ MessageType::STREAM_METADATA };
			uint8_t mediaKind{ 0u };
			uint8_t payloadType{ 0u };
			uint32_t consumerSsrc{ 0u };
			uint32_t producerSsrc{ 0u };
			uint32_t clockRate{ 0u };
			uint32_t pairedConsumerSsrc{ 0u };
			uint32_t pairedProducerSsrc{ 0u };
			uint8_t flags{ 0u };
			uint8_t pairedPayloadType{ 0u };
			uint8_t apt{ 0u };
			uint8_t reserved{ 0u };
			uint16_t consumerIdLength{ 0u };
			uint16_t producerIdLength{ 0u };
			uint16_t codecMimeLength{ 0u };
		};
	#pragma pack(pop)

		struct Counters
		{
			size_t registeredConsumerSsrcs{ 0u };
			size_t announcedConsumerSsrcs{ 0u };
			size_t unknownSsrcCount{ 0u };
			size_t droppedPacketCount{ 0u };
			size_t exportedPacketCount{ 0u };
			size_t metadataMessageCount{ 0u };
			size_t packetMessageCount{ 0u };
			size_t socketWriteFailureCount{ 0u };
		};

	#pragma pack(push, 1)
		struct CountersHeader
		{
			uint8_t version{ 1u };
			MessageType type{ MessageType::EXPORTER_COUNTERS };
			uint16_t reserved{ 0u };
			uint32_t registeredConsumerSsrcs{ 0u };
			uint32_t announcedConsumerSsrcs{ 0u };
			uint32_t unknownSsrcCount{ 0u };
			uint32_t droppedPacketCount{ 0u };
			uint32_t exportedPacketCount{ 0u };
			uint32_t metadataMessageCount{ 0u };
			uint32_t packetMessageCount{ 0u };
			uint32_t socketWriteFailureCount{ 0u };
		};
	#pragma pack(pop)

	public:
		ProbeEgressAdapter();
		explicit ProbeEgressAdapter(const Config& config);
		~ProbeEgressAdapter();

	public:
		bool IsEnabled() const
		{
			return this->config.enabled;
		}
		const Config& GetConfig() const
		{
			return this->config;
		}
		Counters GetCounters() const;
		void RegisterConsumerSsrc(const StreamMetadata& metadata);
		void ExportPacket(const RTC::Consumer* consumer, const RTC::RtpPacket* packet, PacketEventType eventType);
		void ExportPacketData(
		  const std::string& consumerId,
		  uint32_t consumerSsrc,
		  uint16_t sequenceNumber,
		  const uint8_t* data,
		  size_t len,
		  PacketEventType eventType);
		size_t GetUnknownSsrcCount() const
		{
			return this->unknownSsrcCount;
		}

	private:
		bool InitSocket();
		void CloseSocket();
		bool WriteDatagram(const uint8_t* data, size_t len);
		bool EnsureMetadataAnnounced(const StreamMetadata& metadata);
		void MaybeEmitCounters(bool force = false);
		bool SerializeMetadata(const StreamMetadata& metadata);
		void SerializePacketData(
		  const std::string& consumerId,
		  uint32_t consumerSsrc,
		  uint16_t sequenceNumber,
		  const uint8_t* data,
		  size_t len,
		  PacketEventType eventType);

	private:
		Config config;
		int fd{ -1 };
		bool socketReady{ false };
		struct sockaddr_un remoteAddr
		{
		};
		absl::flat_hash_map<uint32_t, StreamMetadata> streamMetadataByConsumerSsrc;
		absl::flat_hash_set<uint32_t> announcedConsumerSsrcs;
		size_t unknownSsrcCount{ 0u };
		size_t droppedPacketCount{ 0u };
		size_t exportedPacketCount{ 0u };
		size_t metadataMessageCount{ 0u };
		size_t packetMessageCount{ 0u };
		size_t socketWriteFailureCount{ 0u };
	};
} // namespace RTC

#endif

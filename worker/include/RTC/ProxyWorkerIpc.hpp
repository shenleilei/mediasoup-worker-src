#ifndef MS_RTC_PROXY_WORKER_IPC_HPP
#define MS_RTC_PROXY_WORKER_IPC_HPP

#include "common.hpp"
#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace RTC
{
	namespace ProxyWorkerIpc
	{
		constexpr size_t kMaxPayloadSize{ 65536u };
		constexpr size_t kHeaderSize{ 12u };
		constexpr size_t kMaxFrameSize{ kHeaderSize + sizeof(sockaddr_storage) + kMaxPayloadSize };

		enum class DecodeError
		{
			None,
			TooSmall,
			BadMagic,
			UnsupportedVersion,
			UnsupportedAddressFamily,
			InvalidAddressLength,
			PayloadTooLarge,
			Truncated,
			TrailingBytes
		};

		struct DecodedFrame
		{
			sockaddr_storage remoteAddr{};
			const uint8_t* payload{ nullptr };
			size_t payloadLen{ 0u };
		};

		inline std::string WorkerSocketPath(const std::string& dir, uint16_t targetPort)
		{
			const bool hasSlash = !dir.empty() && dir.back() == '/';

			return dir + (hasSlash ? "" : "/") + "worker-" + std::to_string(targetPort) + ".sock";
		}

		inline void AppendU16(std::vector<uint8_t>& out, uint16_t value)
		{
			out.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
			out.push_back(static_cast<uint8_t>(value & 0xFFu));
		}

		inline void AppendU32(std::vector<uint8_t>& out, uint32_t value)
		{
			out.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
			out.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
			out.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
			out.push_back(static_cast<uint8_t>(value & 0xFFu));
		}

		inline uint16_t ReadU16(const uint8_t* data)
		{
			return static_cast<uint16_t>(
			  (static_cast<uint16_t>(data[0]) << 8u) | static_cast<uint16_t>(data[1]));
		}

		inline uint32_t ReadU32(const uint8_t* data)
		{
			return (static_cast<uint32_t>(data[0]) << 24u) |
			       (static_cast<uint32_t>(data[1]) << 16u) |
			       (static_cast<uint32_t>(data[2]) << 8u) |
			       static_cast<uint32_t>(data[3]);
		}

		inline bool SockaddrLength(const sockaddr* addr, size_t& outLen)
		{
			if (!addr)
			{
				return false;
			}

			switch (addr->sa_family)
			{
				case AF_INET:
					outLen = sizeof(sockaddr_in);
					return true;

				case AF_INET6:
					outLen = sizeof(sockaddr_in6);
					return true;

				default:
					return false;
			}
		}

		inline bool EncodeFrame(
		  const sockaddr* remoteAddr, const uint8_t* payload, size_t payloadLen, std::vector<uint8_t>& out)
		{
			size_t remoteAddrLen{ 0u };

			if (!SockaddrLength(remoteAddr, remoteAddrLen) || payloadLen > kMaxPayloadSize ||
			    (payloadLen > 0u && payload == nullptr))
			{
				return false;
			}

			out.clear();
			out.reserve(kHeaderSize + remoteAddrLen + payloadLen);
			out.push_back('M');
			out.push_back('S');
			out.push_back('P');
			out.push_back('I');
			out.push_back(1u);
			out.push_back(remoteAddr->sa_family == AF_INET ? 4u : 6u);
			AppendU16(out, static_cast<uint16_t>(remoteAddrLen));
			AppendU32(out, static_cast<uint32_t>(payloadLen));

			const auto* remoteAddrBytes = reinterpret_cast<const uint8_t*>(remoteAddr);
			out.insert(out.end(), remoteAddrBytes, remoteAddrBytes + remoteAddrLen);
			if (payloadLen > 0u)
			{
				out.insert(out.end(), payload, payload + payloadLen);
			}

			return true;
		}

		inline DecodeError DecodeFrame(const uint8_t* data, size_t len, DecodedFrame& out)
		{
			out = {};

			if (!data || len < kHeaderSize)
			{
				return DecodeError::TooSmall;
			}
			if (data[0] != 'M' || data[1] != 'S' || data[2] != 'P' || data[3] != 'I')
			{
				return DecodeError::BadMagic;
			}
			if (data[4] != 1u)
			{
				return DecodeError::UnsupportedVersion;
			}

			const uint8_t family     = data[5];
			const uint16_t addrLen   = ReadU16(data + 6u);
			const uint32_t payloadLen = ReadU32(data + 8u);

			size_t expectedAddrLen{ 0u };
			int expectedFamily{ 0 };
			if (family == 4u)
			{
				expectedFamily  = AF_INET;
				expectedAddrLen = sizeof(sockaddr_in);
			}
			else if (family == 6u)
			{
				expectedFamily  = AF_INET6;
				expectedAddrLen = sizeof(sockaddr_in6);
			}
			else
			{
				return DecodeError::UnsupportedAddressFamily;
			}
			if (addrLen != expectedAddrLen || addrLen > sizeof(sockaddr_storage))
			{
				return DecodeError::InvalidAddressLength;
			}
			if (payloadLen > kMaxPayloadSize)
			{
				return DecodeError::PayloadTooLarge;
			}
			if (len < kHeaderSize + addrLen)
			{
				return DecodeError::Truncated;
			}

			const size_t expectedLen = kHeaderSize + addrLen + payloadLen;
			if (len < expectedLen)
			{
				return DecodeError::Truncated;
			}
			if (len > expectedLen)
			{
				return DecodeError::TrailingBytes;
			}

			std::memcpy(&out.remoteAddr, data + kHeaderSize, addrLen);
			if (reinterpret_cast<const sockaddr*>(&out.remoteAddr)->sa_family != expectedFamily)
			{
				return DecodeError::InvalidAddressLength;
			}

			out.payload    = data + kHeaderSize + addrLen;
			out.payloadLen = payloadLen;

			return DecodeError::None;
		}
	} // namespace ProxyWorkerIpc
} // namespace RTC

#endif

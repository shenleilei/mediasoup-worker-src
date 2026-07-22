#ifndef MS_RTC_TRANSPORT_TUPLE_HPP
#define MS_RTC_TRANSPORT_TUPLE_HPP

#include "common.hpp"
#include "Utils.hpp"
#include "FBS/transport.h"
#include "RTC/TcpConnection.hpp"
#include "RTC/UdpSocket.hpp"
#include <flatbuffers/flatbuffers.h>
#include <string>

namespace RTC
{
	class ProxyWorkerSocket;

	class TransportTuple
	{
	protected:
		using onSendCallback = const std::function<void(bool sent)>;

	public:
		enum class Protocol : uint8_t
		{
			UDP = 1,
			TCP
		};

		static Protocol ProtocolFromFbs(FBS::Transport::Protocol protocol);
		static FBS::Transport::Protocol ProtocolToFbs(Protocol protocol);

	public:
		TransportTuple(RTC::UdpSocket* udpSocket, const struct sockaddr* udpRemoteAddr)
		  : udpSocket(udpSocket), udpRemoteAddr((struct sockaddr*)udpRemoteAddr), protocol(Protocol::UDP)
		{
			SetHash();
		}

		TransportTuple(RTC::ProxyWorkerSocket* proxyWorkerSocket, const struct sockaddr* udpRemoteAddr);

		explicit TransportTuple(RTC::TcpConnection* tcpConnection)
		  : tcpConnection(tcpConnection), protocol(Protocol::TCP)
		{
			SetHash();
		}

		explicit TransportTuple(const TransportTuple* tuple)
		  : hash(tuple->hash), udpSocket(tuple->udpSocket), proxyWorkerSocket(tuple->proxyWorkerSocket),
		    udpRemoteAddr(tuple->udpRemoteAddr), tcpConnection(tuple->tcpConnection),
		    localAnnouncedAddress(tuple->localAnnouncedAddress), protocol(tuple->protocol)
		{
			if (protocol == TransportTuple::Protocol::UDP)
			{
				StoreUdpRemoteAddress();
			}
		}

	public:
		void CloseTcpConnection();

		flatbuffers::Offset<FBS::Transport::Tuple> FillBuffer(flatbuffers::FlatBufferBuilder& builder) const;

		void Dump() const;

		void StoreUdpRemoteAddress()
		{
			// Clone the given address into our address storage and make the sockaddr
			// pointer point to it.
			this->udpRemoteAddrStorage = Utils::IP::CopyAddress(this->udpRemoteAddr);
			this->udpRemoteAddr        = (struct sockaddr*)&this->udpRemoteAddrStorage;
		}

		bool Compare(const TransportTuple* tuple) const
		{
			return this->hash == tuple->hash;
		}

		void SetLocalAnnouncedAddress(std::string& localAnnouncedAddress)
		{
			this->localAnnouncedAddress = localAnnouncedAddress;
		}

		void Send(const uint8_t* data, size_t len, RTC::TransportTuple::onSendCallback* cb = nullptr);

		Protocol GetProtocol() const
		{
			return this->protocol;
		}

		const struct sockaddr* GetLocalAddress() const;

		const struct sockaddr* GetRemoteAddress() const
		{
			if (this->protocol == Protocol::UDP)
			{
				return static_cast<const struct sockaddr*>(this->udpRemoteAddr);
			}
			else
			{
				return this->tcpConnection->GetPeerAddress();
			}
		}

		size_t GetRecvBytes() const;

		size_t GetSentBytes() const;

	private:
		void SetHash();

	public:
		uint64_t hash{ 0u };

	private:
		// Passed by argument.
		RTC::UdpSocket* udpSocket{ nullptr };
		RTC::ProxyWorkerSocket* proxyWorkerSocket{ nullptr };
		struct sockaddr* udpRemoteAddr{ nullptr };
		RTC::TcpConnection* tcpConnection{ nullptr };
		std::string localAnnouncedAddress;
		// Others.
		struct sockaddr_storage udpRemoteAddrStorage
		{
		};
		Protocol protocol;
	};
} // namespace RTC

#endif

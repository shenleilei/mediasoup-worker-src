#include "RTC/SrtpSession.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

TEST_CASE("SRTCP encryption honors the complete trailer capacity", "[srtp][capacity]")
{
	constexpr size_t EncryptBufferSize{ 65536u };
	static_assert(SRTP_MAX_SRTCP_TRAILER_LEN <= EncryptBufferSize);
	constexpr size_t ExactInputSize = EncryptBufferSize - SRTP_MAX_SRTCP_TRAILER_LEN;
	static_assert(ExactInputSize % 4u == 0u);

	std::array<uint8_t, 30u> key{};
	RTC::SrtpSession outbound(
	  RTC::SrtpSession::Type::OUTBOUND,
	  RTC::SrtpSession::CryptoSuite::AES_CM_128_HMAC_SHA1_80,
	  key.data(),
	  key.size());
	RTC::SrtpSession inbound(
	  RTC::SrtpSession::Type::INBOUND,
	  RTC::SrtpSession::CryptoSuite::AES_CM_128_HMAC_SHA1_80,
	  key.data(),
	  key.size());

	std::vector<uint8_t> exactPacket(ExactInputSize, 0u);
	exactPacket[0] = 0x80u;
	exactPacket[1] = 200u; // Sender Report.
	const auto lengthInWords = static_cast<uint16_t>(ExactInputSize / 4u - 1u);
	exactPacket[2] = static_cast<uint8_t>(lengthInWords >> 8u);
	exactPacket[3] = static_cast<uint8_t>(lengthInWords & 0xffu);
	exactPacket[7] = 1u; // Non-zero SSRC.

	const uint8_t* encryptedData = exactPacket.data();
	size_t encryptedLen          = exactPacket.size();
	REQUIRE(outbound.EncryptRtcp(&encryptedData, &encryptedLen));
	CHECK(encryptedData != exactPacket.data());
	CHECK(encryptedLen > exactPacket.size());
	CHECK(encryptedLen <= EncryptBufferSize);

	std::vector<uint8_t> decrypted(encryptedData, encryptedData + encryptedLen);
	size_t decryptedLen = decrypted.size();
	REQUIRE(inbound.DecryptSrtcp(decrypted.data(), &decryptedLen));
	CHECK(decryptedLen == exactPacket.size());
	CHECK(std::memcmp(decrypted.data(), exactPacket.data(), exactPacket.size()) == 0);

	std::vector<uint8_t> oversizedPacket(ExactInputSize + 1u, 0u);
	const uint8_t* rejectedData = oversizedPacket.data();
	size_t rejectedLen          = oversizedPacket.size();
	CHECK_FALSE(outbound.EncryptRtcp(&rejectedData, &rejectedLen));
	CHECK(rejectedData == oversizedPacket.data());
	CHECK(rejectedLen == oversizedPacket.size());
}

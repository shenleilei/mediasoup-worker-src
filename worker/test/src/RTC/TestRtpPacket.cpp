#include "common.hpp"
#include "helpers.hpp"
#include "RTC/RtpPacket.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring> // std::memset()
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace RTC;

static uint8_t buffer[65536];

SCENARIO("parse RTP packets", "[parser][rtp]")
{
	SECTION("parse packet1.raw")
	{
		size_t len;
		uint8_t extenLen;
		uint8_t* extenValue;
		std::string rid;

		if (!helpers::readBinaryFile("data/packet1.raw", buffer, &len))
		{
			FAIL("cannot open file");
		}

		RtpPacket* packet = RtpPacket::Parse(buffer, len);

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetPayloadType() == 111);
		REQUIRE(packet->GetSequenceNumber() == 23617);
		REQUIRE(packet->GetTimestamp() == 1660241882);
		REQUIRE(packet->GetSsrc() == 2674985186);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 4);
		REQUIRE(packet->HasOneByteExtensions());
		REQUIRE(packet->HasTwoBytesExtensions() == false);

		packet->SetRidExtensionId(10);
		extenValue = packet->GetExtension(10, extenLen);

		REQUIRE(packet->HasExtension(10) == false);
		REQUIRE(extenLen == 0);
		REQUIRE(extenValue == nullptr);
		REQUIRE(packet->ReadRid(rid) == false);
		REQUIRE(rid == "");

		delete packet;
	}

	SECTION("parse packet2.raw")
	{
		size_t len;

		if (!helpers::readBinaryFile("data/packet2.raw", buffer, &len))
		{
			FAIL("cannot open file");
		}

		RtpPacket* packet = RtpPacket::Parse(buffer, len);

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == false);
		REQUIRE(packet->GetPayloadType() == 100);
		REQUIRE(packet->GetSequenceNumber() == 28478);
		REQUIRE(packet->GetTimestamp() == 172320136);
		REQUIRE(packet->GetSsrc() == 3316375386);
		REQUIRE(packet->GetHeaderExtensionId() == 0);
		REQUIRE(packet->GetHeaderExtensionLength() == 0);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == false);

		delete packet;
	}

	SECTION("parse packet3.raw")
	{
		size_t len;
		uint8_t extenLen;
		uint8_t* extenValue;
		bool voice;
		uint8_t volume;
		uint32_t absSendTime;
		if (!helpers::readBinaryFile("data/packet3.raw", buffer, &len))
		{
			FAIL("cannot open file");
		}

		RtpPacket* packet = RtpPacket::Parse(buffer, len);

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetPayloadType() == 111);
		REQUIRE(packet->GetSequenceNumber() == 19354);
		REQUIRE(packet->GetTimestamp() == 863466045);
		REQUIRE(packet->GetSsrc() == 235797202);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 8);
		REQUIRE(packet->HasOneByteExtensions());
		REQUIRE(packet->HasTwoBytesExtensions() == false);

		packet->SetSsrcAudioLevelExtensionId(1);
		extenValue = packet->GetExtension(1, extenLen);

		REQUIRE(packet->HasExtension(1) == true);
		REQUIRE(extenLen == 1);
		REQUIRE(extenValue);
		REQUIRE(extenValue[0] == 0xd0);
		REQUIRE(packet->ReadSsrcAudioLevel(volume, voice) == true);
		REQUIRE(volume == 0b1010000);
		REQUIRE(voice == true);

		packet->SetAbsSendTimeExtensionId(3);
		extenValue = packet->GetExtension(3, extenLen);

		REQUIRE(packet->HasExtension(3) == true);
		REQUIRE(extenLen == 3);
		REQUIRE(extenValue);
		REQUIRE(extenValue[0] == 0x65);
		REQUIRE(extenValue[1] == 0x34);
		REQUIRE(extenValue[2] == 0x1e);
		REQUIRE(packet->ReadAbsSendTime(absSendTime) == true);
		REQUIRE(absSendTime == 0x65341e);

		auto* clonedPacket = packet->Clone();

		std::memset(buffer, '0', sizeof(buffer));

		REQUIRE(clonedPacket->HasMarker() == false);
		REQUIRE(clonedPacket->HasHeaderExtension() == true);
		REQUIRE(clonedPacket->GetPayloadType() == 111);
		REQUIRE(clonedPacket->GetSequenceNumber() == 19354);
		REQUIRE(clonedPacket->GetTimestamp() == 863466045);
		REQUIRE(clonedPacket->GetSsrc() == 235797202);
		REQUIRE(clonedPacket->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(clonedPacket->GetHeaderExtensionLength() == 8);
		REQUIRE(clonedPacket->HasOneByteExtensions());
		REQUIRE(clonedPacket->HasTwoBytesExtensions() == false);

		extenValue = clonedPacket->GetExtension(1, extenLen);

		REQUIRE(packet->HasExtension(1) == false);
		REQUIRE(extenLen == 1);
		REQUIRE(extenValue);
		REQUIRE(extenValue[0] == 0xd0);
		REQUIRE(clonedPacket->ReadSsrcAudioLevel(volume, voice) == true);
		REQUIRE(volume == 0b1010000);
		REQUIRE(voice == true);

		extenValue = clonedPacket->GetExtension(3, extenLen);

		REQUIRE(packet->HasExtension(3) == false);
		REQUIRE(extenLen == 3);
		REQUIRE(extenValue);
		REQUIRE(extenValue[0] == 0x65);
		REQUIRE(extenValue[1] == 0x34);
		REQUIRE(extenValue[2] == 0x1e);
		REQUIRE(clonedPacket->ReadAbsSendTime(absSendTime) == true);
		REQUIRE(absSendTime == 0x65341e);

		delete packet;
		delete clonedPacket;
	}

	SECTION("create RtpPacket without header extension")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0x80, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == false);
		REQUIRE(packet->GetPayloadType() == 1);
		REQUIRE(packet->GetSequenceNumber() == 8);
		REQUIRE(packet->GetTimestamp() == 4);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetSsrc() == 5);

		delete packet;
	}

	SECTION("create RtpPacket with One-Byte header extension")
	{
		// clang-format off
		uint8_t buffer[1032] =
		{
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0xbe, 0xde, 0x00, 0x03, // Header Extension
			0x10, 0xff, 0x21, 0xff,
			0xff, 0x00, 0x00, 0x33,
			0xff, 0xff, 0xff, 0xff
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, 28u, sizeof(buffer));

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetPayloadType() == 1);
		REQUIRE(packet->GetSequenceNumber() == 8);
		REQUIRE(packet->GetTimestamp() == 4);
		REQUIRE(packet->GetSsrc() == 5);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 12);
		REQUIRE(packet->HasOneByteExtensions());
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 0);
		REQUIRE(packet->GetSize() == 28);

		REQUIRE(packet->SetPayloadLength(1000));

		REQUIRE(packet->GetPayloadLength() == 1000);
		REQUIRE(packet->GetSize() == 1028);

		delete packet;
	}

	SECTION("create RtpPacket with One-Byte abs-capture-time header extension")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0xbe, 0xde, 0x00, 0x03, // Header Extension (12 bytes)
			0x47,                   // id=4 len=8
			0x01, 0x02, 0x03, 0x04,
			0x05, 0x06, 0x07, 0x08,
			0x00, 0x00, 0x00
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));
		uint8_t extenLen{ 0u };
		uint8_t* extenValue{ nullptr };
		uint64_t absoluteCaptureTimestamp{ 0u };
		bool hasEstimatedCaptureClockOffset2{ false };
		int64_t estimatedCaptureClockOffset2{ 0 };

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->HasOneByteExtensions() == true);
		REQUIRE(packet->HasTwoBytesExtensions() == false);

		packet->SetAbsCaptureTimeExtensionId(4);
		extenValue = packet->GetExtension(4, extenLen);

		REQUIRE(packet->HasExtension(4) == true);
		REQUIRE(extenLen == 8);
		REQUIRE(extenValue);
		REQUIRE(
		  packet->ReadAbsCaptureTime(
		    absoluteCaptureTimestamp, hasEstimatedCaptureClockOffset2, estimatedCaptureClockOffset2) ==
		  true);
		REQUIRE(absoluteCaptureTimestamp == 0x0102030405060708ULL);
		REQUIRE(hasEstimatedCaptureClockOffset2 == false);
		REQUIRE(estimatedCaptureClockOffset2 == 0);

		delete packet;
	}

	SECTION("create RtpPacket with Two-Bytes abs-capture-time header extension")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0x10, 0x00, 0x00, 0x05, // Header Extension (two-byte, 20 bytes)
			0x0d, 0x10,             // id=13 len=16
			0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
			0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8,
			0x00, 0x00
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));
		uint64_t absoluteCaptureTimestamp{ 0u };
		bool hasEstimatedCaptureClockOffset2{ false };
		int64_t estimatedCaptureClockOffset2{ 0 };

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == true);

		packet->SetAbsCaptureTimeExtensionId(13);
		REQUIRE(
		  packet->ReadAbsCaptureTime(
		    absoluteCaptureTimestamp, hasEstimatedCaptureClockOffset2, estimatedCaptureClockOffset2) ==
		  true);
		REQUIRE(absoluteCaptureTimestamp == 0x0102030405060708ULL);
		REQUIRE(hasEstimatedCaptureClockOffset2 == true);
		REQUIRE(static_cast<uint64_t>(estimatedCaptureClockOffset2) == 0xfffefdfcfbfaf9f8ULL);

		delete packet;
	}

	SECTION("updates Two-Bytes abs-capture-time clock offset in place")
	{
		uint8_t buffer[] = {
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0x10, 0x00, 0x00, 0x05,
			0x0d, 0x10,
			0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
			0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8,
			0x00, 0x00,
		};
		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));
		REQUIRE(packet != nullptr);
		packet->SetAbsCaptureTimeExtensionId(13);

		uint64_t captureTimestamp{ 0u };
		bool hasOffset{ false };
		int64_t before{ 0 };
		REQUIRE(packet->ReadAbsCaptureTime(captureTimestamp, hasOffset, before));
		REQUIRE(hasOffset);
		REQUIRE(packet->UpdateAbsCaptureTimeOffsetMs(500));

		int64_t after{ 0 };
		REQUIRE(packet->ReadAbsCaptureTime(captureTimestamp, hasOffset, after));
		REQUIRE(hasOffset);
		REQUIRE(Utils::Time::SignedNtp64ToMs(after) - Utils::Time::SignedNtp64ToMs(before) == 500);
		delete packet;
	}

	SECTION("create RtpPacket with Two-Bytes header extension")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0x10, 0x00, 0x00, 0x04, // Header Extension
			0x00, 0x00, 0x01, 0x00,
			0x02, 0x01, 0x42, 0x00,
			0x03, 0x02, 0x11, 0x22,
			0x00, 0x00, 0x04, 0x00
		};
		// clang-format on

		uint8_t extenLen;
		uint8_t* extenValue;

		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetPayloadType() == 1);
		REQUIRE(packet->GetSequenceNumber() == 8);
		REQUIRE(packet->GetTimestamp() == 4);
		REQUIRE(packet->GetSsrc() == 5);
		REQUIRE(packet->GetHeaderExtensionLength() == 16);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions());
		REQUIRE(packet->GetPayloadLength() == 0);

		extenValue = packet->GetExtension(1, extenLen);
		REQUIRE(packet->HasExtension(1) == false);
		REQUIRE(extenValue == nullptr);
		REQUIRE(extenLen == 0);

		extenValue = packet->GetExtension(2, extenLen);
		REQUIRE(packet->HasExtension(2) == true);
		REQUIRE(extenValue != nullptr);
		REQUIRE(extenLen == 1);
		REQUIRE(extenValue[0] == 0x42);

		extenValue = packet->GetExtension(3, extenLen);
		REQUIRE(packet->HasExtension(3) == true);
		REQUIRE(extenValue != nullptr);
		REQUIRE(extenLen == 2);
		REQUIRE(extenValue[0] == 0x11);
		REQUIRE(extenValue[1] == 0x22);

		extenValue = packet->GetExtension(4, extenLen);
		REQUIRE(packet->HasExtension(4) == false);
		REQUIRE(extenValue == nullptr);
		REQUIRE(extenLen == 0);

		extenValue = packet->GetExtension(5, extenLen);
		REQUIRE(packet->HasExtension(5) == false);
		REQUIRE(extenValue == nullptr);
		REQUIRE(extenLen == 0);

		delete packet;
	}

	SECTION("rtx encryption-decryption")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0x10, 0x00, 0x00, 0x03, // Header Extension
			0x01, 0x00, 0x02, 0x01,
			0xff, 0x00, 0x03, 0x04,
			0xff, 0xff, 0xff, 0xff,
			0x11, 0x11, 0x11, 0x11 // payload
		};
		// clang-format on

		uint8_t rtxPayloadType{ 102 };
		uint32_t rtxSsrc{ 6 };
		uint16_t rtxSeq{ 80 };

		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetPayloadType() == 1);
		REQUIRE(packet->GetSequenceNumber() == 8);
		REQUIRE(packet->GetTimestamp() == 4);
		REQUIRE(packet->GetSsrc() == 5);
		REQUIRE(packet->GetPayloadLength() == 4);
		REQUIRE(packet->GetHeaderExtensionLength() == 12);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions());

		auto* rtxPacket = packet->Clone();

		delete packet;

		std::memset(buffer, '0', sizeof(buffer));

		REQUIRE(rtxPacket->RtxEncode(rtxPayloadType, rtxSsrc, rtxSeq));

		REQUIRE(rtxPacket->HasMarker() == false);
		REQUIRE(rtxPacket->HasHeaderExtension() == true);
		REQUIRE(rtxPacket->GetPayloadType() == rtxPayloadType);
		REQUIRE(rtxPacket->GetSequenceNumber() == rtxSeq);
		REQUIRE(rtxPacket->GetTimestamp() == 4);
		REQUIRE(rtxPacket->GetSsrc() == rtxSsrc);
		REQUIRE(rtxPacket->GetPayloadLength() == 6);
		REQUIRE(rtxPacket->GetHeaderExtensionLength() == 12);
		REQUIRE(rtxPacket->HasOneByteExtensions() == false);
		REQUIRE(rtxPacket->HasTwoBytesExtensions());

		rtxPacket->RtxDecode(1, 5);

		REQUIRE(rtxPacket->HasMarker() == false);
		REQUIRE(rtxPacket->HasHeaderExtension() == true);
		REQUIRE(rtxPacket->GetPayloadType() == 1);
		REQUIRE(rtxPacket->GetSequenceNumber() == 8);
		REQUIRE(rtxPacket->GetTimestamp() == 4);
		REQUIRE(rtxPacket->GetSsrc() == 5);
		REQUIRE(rtxPacket->GetPayloadLength() == 4);
		REQUIRE(rtxPacket->GetHeaderExtensionLength() == 12);
		REQUIRE(rtxPacket->HasOneByteExtensions() == false);
		REQUIRE(rtxPacket->HasTwoBytesExtensions());

		delete rtxPacket;
	}

	SECTION("create RtpPacket and apply payload shift to it")
	{
		// clang-format off
		uint8_t buffer[1040] =
		{
			0xb0, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0xbe, 0xde, 0x00, 0x03, // Header Extension
			0x10, 0xff, 0x21, 0xff,
			0xff, 0x00, 0x00, 0x33,
			0xff, 0xff, 0xff, 0xff,
			0x00, 0x01, 0x02, 0x03, // Payload
			0x04, 0x05, 0x06, 0x07,
			0x00, 0x00, 0x00, 0x04, // 4 padding bytes
			0x00, 0x00, 0x00, 0x00, // Free buffer
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		// clang-format on

		size_t len        = 40;
		RtpPacket* packet = RtpPacket::Parse(buffer, len, sizeof(buffer));

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->GetPayloadType() == 1);
		REQUIRE(packet->GetSequenceNumber() == 8);
		REQUIRE(packet->GetTimestamp() == 4);
		REQUIRE(packet->GetSsrc() == 5);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 12);
		REQUIRE(packet->HasOneByteExtensions());
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 8);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetSize() == 40);

		auto* payload = packet->GetPayload();

		REQUIRE(payload[0] == 0x00);
		REQUIRE(payload[1] == 0x01);
		REQUIRE(payload[2] == 0x02);
		REQUIRE(payload[3] == 0x03);
		REQUIRE(payload[4] == 0x04);
		REQUIRE(payload[5] == 0x05);
		REQUIRE(payload[6] == 0x06);
		REQUIRE(payload[7] == 0x07);

		// NOTE: This will remove padding.
		REQUIRE(packet->ShiftPayload(0, 2, true));

		REQUIRE(packet->GetPayloadLength() == 10);
		REQUIRE(packet->GetPayloadPadding() == 0);
		REQUIRE(packet->GetSize() == 38);
		REQUIRE(payload[2] == 0x00);
		REQUIRE(payload[3] == 0x01);
		REQUIRE(payload[4] == 0x02);
		REQUIRE(payload[5] == 0x03);
		REQUIRE(payload[6] == 0x04);
		REQUIRE(payload[7] == 0x05);
		REQUIRE(payload[8] == 0x06);
		REQUIRE(payload[9] == 0x07);

		REQUIRE(packet->ShiftPayload(0, 2, false));

		REQUIRE(packet->GetPayloadLength() == 8);
		REQUIRE(packet->GetPayloadPadding() == 0);
		REQUIRE(packet->GetSize() == 36);
		REQUIRE(payload[0] == 0x00);
		REQUIRE(payload[1] == 0x01);
		REQUIRE(payload[2] == 0x02);
		REQUIRE(payload[3] == 0x03);
		REQUIRE(payload[4] == 0x04);
		REQUIRE(payload[5] == 0x05);
		REQUIRE(payload[6] == 0x06);
		REQUIRE(payload[7] == 0x07);

		// NOTE: This will remove padding.
		REQUIRE(packet->SetPayloadLength(14));

		REQUIRE(packet->GetPayloadLength() == 14);
		REQUIRE(packet->GetPayloadPadding() == 0);
		REQUIRE(packet->GetSize() == 42);

		REQUIRE(packet->ShiftPayload(4, 4, true));

		REQUIRE(packet->GetPayloadLength() == 18);
		REQUIRE(packet->GetPayloadPadding() == 0);
		REQUIRE(packet->GetSize() == 46);
		REQUIRE(payload[0] == 0x00);
		REQUIRE(payload[1] == 0x01);
		REQUIRE(payload[2] == 0x02);
		REQUIRE(payload[3] == 0x03);
		REQUIRE(payload[8] == 0x04);
		REQUIRE(payload[9] == 0x05);
		REQUIRE(payload[10] == 0x06);
		REQUIRE(payload[11] == 0x07);

		REQUIRE(packet->SetPayloadLength(1000));

		REQUIRE(packet->GetPayloadLength() == 1000);
		REQUIRE(packet->GetPayloadPadding() == 0);
		REQUIRE(packet->GetSize() == 1028);

		delete packet;
	}

	SECTION("set One-Byte header extensions")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0xa0, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0x11, 0x22, 0x33, 0x44, // Payload
			0x55, 0x66, 0x77, 0x88,
			0x99, 0xaa, 0xbb, 0xcc,
			0x00, 0x00, 0x00, 0x04, // 4 padding bytes
			// Extra buffer
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, 28, sizeof(buffer));
		std::vector<RTC::RtpPacket::GenericExtension> extensions;
		uint8_t extenLen;
		uint8_t* extenValue;

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->GetSize() == 28);
		REQUIRE(packet->HasHeaderExtension() == false);
		REQUIRE(packet->GetHeaderExtensionId() == 0);
		REQUIRE(packet->GetHeaderExtensionLength() == 0);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);

		extensions.clear();

		packet->SetExtensions(1, extensions);

		REQUIRE(packet->GetSize() == 32);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 0);
		REQUIRE(packet->HasOneByteExtensions() == true);
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);

		extensions.clear();

		uint8_t value1[] = { 0x01, 0x02, 0x03, 0x04 };

		// This must be ignored due to id=0.
		extensions.emplace_back(
		  0,     // id
		  4,     // len
		  value1 // value
		);

		// This must be ignored due to id>14.
		extensions.emplace_back(
		  15,    // id
		  4,     // len
		  value1 // value
		);

		// This must be ignored due to id>14.
		extensions.emplace_back(
		  22,    // id
		  4,     // len
		  value1 // value
		);

		extensions.emplace_back(
		  1,     // id
		  4,     // len
		  value1 // value
		);

		uint8_t value2[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11 };

		extensions.emplace_back(
		  2,     // id
		  11,    // len
		  value2 // value
		);

		packet->SetExtensions(1, extensions);

		REQUIRE(packet->GetSize() == 52); // 49 + 3 bytes for padding in header extension.
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 20); // 17 + 3 bytes for padding.
		REQUIRE(packet->HasOneByteExtensions() == true);
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);
		REQUIRE(packet->GetExtension(0, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(0) == false);
		REQUIRE(packet->GetExtension(15, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(15) == false);
		REQUIRE(packet->GetExtension(22, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(22) == false);
		REQUIRE(packet->GetExtension(1, extenLen));
		REQUIRE(packet->HasExtension(1) == true);
		REQUIRE(extenLen == 4);
		REQUIRE(packet->GetExtension(2, extenLen));
		REQUIRE(packet->HasExtension(2) == true);
		REQUIRE(extenLen == 11);

		extensions.clear();

		uint8_t value3[] = { 0x01, 0x02, 0x03, 0x04 };

		extensions.emplace_back(
		  14,    // id
		  4,     // len
		  value3 // value
		);

		packet->SetExtensions(1, extensions);

		REQUIRE(packet->GetSize() == 40);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 8); // 5 + 3 bytes for padding.
		REQUIRE(packet->HasOneByteExtensions() == true);
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);
		REQUIRE(packet->GetExtension(1, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(1) == false);
		REQUIRE(packet->GetExtension(2, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(2) == false);
		REQUIRE((extenValue = packet->GetExtension(14, extenLen)));
		REQUIRE(packet->HasExtension(14) == true);
		REQUIRE(extenLen == 4);
		REQUIRE(extenValue[0] == 0x01);
		REQUIRE(extenValue[1] == 0x02);
		REQUIRE(extenValue[2] == 0x03);
		REQUIRE(extenValue[3] == 0x04);
		REQUIRE(packet->SetExtensionLength(14, 3) == true);
		REQUIRE((extenValue = packet->GetExtension(14, extenLen)));
		REQUIRE(packet->HasExtension(14) == true);
		REQUIRE(extenLen == 3);
		REQUIRE(extenValue[0] == 0x01);
		REQUIRE(extenValue[1] == 0x02);
		REQUIRE(extenValue[2] == 0x03);
		REQUIRE(extenValue[3] == 0x00);

		delete packet;
	}

	SECTION("set Two-Bytes header extensions")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0xa0, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0x11, 0x22, 0x33, 0x44, // Payload
			0x55, 0x66, 0x77, 0x88,
			0x99, 0xaa, 0xbb, 0xcc,
			0x00, 0x00, 0x00, 0x04, // 4 padding bytes
			// Extra buffer
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, 28, sizeof(buffer));
		std::vector<RTC::RtpPacket::GenericExtension> extensions;
		uint8_t extenLen;
		uint8_t* extenValue;

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->GetSize() == 28);
		REQUIRE(packet->HasHeaderExtension() == false);
		REQUIRE(packet->GetHeaderExtensionId() == 0);
		REQUIRE(packet->GetHeaderExtensionLength() == 0);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);

		extensions.clear();

		packet->SetExtensions(2, extensions);

		REQUIRE(packet->GetSize() == 32);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0b0001000000000000);
		REQUIRE(packet->GetHeaderExtensionLength() == 0);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == true);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);

		extensions.clear();

		uint8_t value1[] = { 0x01, 0x02, 0x03, 0x04 };

		// This must be ignored due to id=0.
		extensions.emplace_back(
		  0,     // id
		  4,     // len
		  value1 // value
		);

		extensions.emplace_back(
		  1,     // id
		  4,     // len
		  value1 // value
		);

		uint8_t value2[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11 };

		extensions.emplace_back(
		  22,    // id
		  11,    // len
		  value2 // value
		);

		packet->SetExtensions(2, extensions);

		REQUIRE(packet->GetSize() == 52); // 51 + 1 byte for padding in header extension.
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0b0001000000000000);
		REQUIRE(packet->GetHeaderExtensionLength() == 20); // 19 + 1 byte for padding.
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == true);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);
		REQUIRE(packet->GetExtension(0, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(0) == false);
		REQUIRE((extenValue = packet->GetExtension(1, extenLen)));
		REQUIRE(packet->HasExtension(1) == true);
		REQUIRE(extenLen == 4);
		REQUIRE(extenValue[0] == 0x01);
		REQUIRE(extenValue[1] == 0x02);
		REQUIRE(extenValue[2] == 0x03);
		REQUIRE(extenValue[3] == 0x04);
		REQUIRE(packet->SetExtensionLength(1, 2) == true);
		REQUIRE((extenValue = packet->GetExtension(1, extenLen)));
		REQUIRE(packet->HasExtension(1) == true);
		REQUIRE(extenLen == 2);
		REQUIRE(extenValue[0] == 0x01);
		REQUIRE(extenValue[1] == 0x02);
		REQUIRE(extenValue[2] == 0x00);
		REQUIRE(extenValue[3] == 0x00);
		REQUIRE(packet->GetExtension(22, extenLen));
		REQUIRE(packet->HasExtension(22) == true);
		REQUIRE(extenLen == 11);

		extensions.clear();

		uint8_t value3[] = { 0x01, 0x02, 0x03, 0x04 };

		extensions.emplace_back(
		  24,    // id
		  4,     // len
		  value3 // value
		);

		packet->SetExtensions(2, extensions);

		REQUIRE(packet->GetSize() == 40);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetHeaderExtensionId() == 0b0001000000000000);
		REQUIRE(packet->GetHeaderExtensionLength() == 8);
		REQUIRE(packet->HasOneByteExtensions() == false);
		REQUIRE(packet->HasTwoBytesExtensions() == true);
		REQUIRE(packet->GetPayloadLength() == 12);
		REQUIRE(packet->GetPayloadPadding() == 4);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() + packet->GetPayloadPadding() - 1] == 4);
		REQUIRE(packet->GetPayload()[0] == 0x11);
		REQUIRE(packet->GetPayload()[packet->GetPayloadLength() - 1] == 0xCC);
		REQUIRE(packet->GetExtension(1, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(1) == false);
		REQUIRE(packet->GetExtension(22, extenLen) == nullptr);
		REQUIRE(packet->HasExtension(22) == false);
		REQUIRE(packet->GetExtension(24, extenLen));
		REQUIRE(packet->HasExtension(24) == true);
		REQUIRE(extenLen == 4);

		delete packet;
	}

	SECTION("read frame-marking extension")
	{
		// clang-format off
		uint8_t buffer[] =
		{
			0x90, 0x01, 0x00, 0x08,
			0x00, 0x00, 0x00, 0x04,
			0x00, 0x00, 0x00, 0x05,
			0xbe, 0xde, 0x00, 0x01, // Header Extension
			0x32, 0xab, 0x01, 0x05,
			0x01, 0x02, 0x03, 0x04
		};
		// clang-format on

		RtpPacket* packet = RtpPacket::Parse(buffer, sizeof(buffer));

		if (!packet)
		{
			FAIL("not a RTP packet");
		}

		REQUIRE(packet->HasMarker() == false);
		REQUIRE(packet->HasHeaderExtension() == true);
		REQUIRE(packet->GetPayloadType() == 1);
		REQUIRE(packet->GetSequenceNumber() == 8);
		REQUIRE(packet->GetTimestamp() == 4);
		REQUIRE(packet->GetSsrc() == 5);
		REQUIRE(packet->GetHeaderExtensionId() == 0xBEDE);
		REQUIRE(packet->GetHeaderExtensionLength() == 4);
		REQUIRE(packet->HasOneByteExtensions());
		REQUIRE(packet->HasTwoBytesExtensions() == false);
		REQUIRE(packet->GetPayloadLength() == 4);

		packet->SetFrameMarkingExtensionId(3);

		RtpPacket::FrameMarking* frameMarking;
		uint8_t frameMarkingLen;

		REQUIRE(packet->ReadFrameMarking(&frameMarking, frameMarkingLen) == true);
		REQUIRE(frameMarkingLen == 3);
		REQUIRE(frameMarking->start == 1);
		REQUIRE(frameMarking->end == 0);
		REQUIRE(frameMarking->independent == 1);
		REQUIRE(frameMarking->discardable == 0);
		REQUIRE(frameMarking->base == 1);
		REQUIRE(frameMarking->tid == 3);
		REQUIRE(frameMarking->lid == 1);
		REQUIRE(frameMarking->tl0picidx == 5);

		delete packet;
	}
}

TEST_CASE("RTP extension growth is fail-closed and state preserving", "[rtp][capacity]")
{
	for (size_t iteration = 0; iteration < 256; ++iteration)
	{
		std::array<uint8_t, 64> storage{};
		storage[0]  = 0x81; // RTP version 2 with one CSRC (keeps IsRtp's mux guard happy).
		storage[1]  = 96;
		storage[2]  = static_cast<uint8_t>(iteration);
		storage[3]  = static_cast<uint8_t>(iteration >> 8);
		storage[11] = 1;
		for (size_t i = 12; i < 20; ++i)
		{
			storage[i] = static_cast<uint8_t>(i + iteration);
		}

		const size_t packetLength     = 20;
		const size_t capacity         = packetLength + (iteration % 33);
		const uint8_t extensionLength = static_cast<uint8_t>((iteration % 16) + 1);
		std::array<uint8_t, 16> extensionValue{};
		for (size_t i = 0; i < extensionValue.size(); ++i)
		{
			extensionValue[i] = static_cast<uint8_t>(i ^ iteration);
		}
		std::vector<RtpPacket::GenericExtension> extensions{ RtpPacket::GenericExtension(
			static_cast<uint8_t>((iteration % 14) + 1), extensionLength, extensionValue.data()) };

		const auto before = storage;
		RtpPacket* packet = RtpPacket::Parse(storage.data(), packetLength, capacity);
		REQUIRE(packet != nullptr);
		const bool changed                  = packet->SetExtensions(1, extensions);
		const size_t expectedExtensionBytes = ((1u + extensionLength + 3u) / 4u) * 4u + 4u;
		if (capacity < packetLength + expectedExtensionBytes)
		{
			REQUIRE_FALSE(changed);
			REQUIRE(storage == before);
			REQUIRE(packet->GetSize() == packetLength);
			REQUIRE_FALSE(packet->HasHeaderExtension());
		}
		else
		{
			REQUIRE(changed);
			REQUIRE(packet->GetSize() <= capacity);
			REQUIRE(packet->HasHeaderExtension());
		}
		delete packet;
	}

	std::array<uint8_t, 32> storage{};
	storage[0]        = 0x81;
	storage[1]        = 96;
	RtpPacket* packet = RtpPacket::Parse(storage.data(), 20, storage.size());
	REQUIRE(packet != nullptr);
	const auto before = storage;
	std::vector<RtpPacket::GenericExtension> invalid{ RtpPacket::GenericExtension(1, 4, nullptr) };
	REQUIRE_FALSE(packet->SetExtensions(1, invalid));
	REQUIRE(storage == before);
	delete packet;
}

TEST_CASE("SetExtensions clears stale abs-capture-time metadata", "[rtp][extensions]")
{
	std::array<uint8_t, 64u> storage{};
	storage[0]  = 0x80u;
	storage[1]  = 96u;
	storage[11] = 1u;
	std::unique_ptr<RtpPacket> packet(
	  RtpPacket::Parse(storage.data(), RtpPacket::HeaderSize, storage.size()));
	REQUIRE(packet);

	std::array<uint8_t, 8u> absCaptureTime{ 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
	std::vector<RtpPacket::GenericExtension> initial{ RtpPacket::GenericExtension(
		6u, static_cast<uint8_t>(absCaptureTime.size()), absCaptureTime.data()) };
	REQUIRE(packet->SetExtensions(1u, initial));
	packet->SetAbsCaptureTimeExtensionId(6u);

	uint64_t absoluteCaptureTimestamp{ 0u };
	bool hasEstimatedCaptureClockOffset{ false };
	int64_t estimatedCaptureClockOffset{ 0 };
	REQUIRE(packet->ReadAbsCaptureTime(
	  absoluteCaptureTimestamp, hasEstimatedCaptureClockOffset, estimatedCaptureClockOffset));
	CHECK(absoluteCaptureTimestamp == 0x0102030405060708ULL);

	std::array<uint8_t, 8u> unrelatedValue{ 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u };
	std::vector<RtpPacket::GenericExtension> replacement{ RtpPacket::GenericExtension(
		6u, static_cast<uint8_t>(unrelatedValue.size()), unrelatedValue.data()) };
	REQUIRE(packet->SetExtensions(1u, replacement));
	CHECK_FALSE(packet->ReadAbsCaptureTime(
	  absoluteCaptureTimestamp, hasEstimatedCaptureClockOffset, estimatedCaptureClockOffset));
}

TEST_CASE("RtpPacket growth operations fail transactionally at capacity", "[rtp][capacity]")
{
	std::array<uint8_t, 20u> storage{};
	storage[0]  = 0x80u;
	storage[1]  = 96u;
	storage[2]  = 0x12u;
	storage[3]  = 0x34u;
	storage[11] = 1u;
	for (size_t i{ RtpPacket::HeaderSize }; i < storage.size(); ++i)
	{
		storage[i] = static_cast<uint8_t>(i);
	}

	std::unique_ptr<RtpPacket> packet(RtpPacket::Parse(storage.data(), storage.size(), storage.size()));
	REQUIRE(packet);
	const auto before              = storage;
	const auto* dataBefore         = packet->GetData();
	auto* payloadBefore            = packet->GetPayload();
	const auto payloadLengthBefore = packet->GetPayloadLength();
	const auto paddingBefore       = packet->GetPayloadPadding();
	const bool hadExtensions       = packet->HasHeaderExtension();

	CHECK_FALSE(packet->SetPayloadLength(packet->GetPayloadLength() + 1u));
	CHECK_FALSE(packet->SetPayloadLength(std::numeric_limits<size_t>::max()));
	CHECK_FALSE(packet->ShiftPayload(2u, 1u, true));
	CHECK_FALSE(packet->ShiftPayload(2u, std::numeric_limits<size_t>::max(), true));
	CHECK_FALSE(packet->RtxEncode(97u, 22u, 33u));
	CHECK_FALSE(packet->ShiftPayload(packet->GetPayloadLength(), 1u, false));
	CHECK(storage == before);
	CHECK(packet->GetData() == dataBefore);
	CHECK(packet->GetPayload() == payloadBefore);
	CHECK(packet->GetPayloadLength() == payloadLengthBefore);
	CHECK(packet->GetPayloadPadding() == paddingBefore);
	CHECK(packet->HasHeaderExtension() == hadExtensions);
	CHECK(packet->GetSize() == storage.size());
	CHECK(packet->GetPayloadType() == 96u);
	CHECK(packet->GetSequenceNumber() == 0x1234u);
	CHECK(packet->GetSsrc() == 1u);
}

TEST_CASE("RtpPacket growth operations accept exact capacity", "[rtp][capacity]")
{
	auto makePacket = [](uint8_t* data, size_t len, size_t capacity)
	{
		std::fill(data, data + capacity, 0u);
		data[0]  = 0x80u;
		data[1]  = 96u;
		data[11] = 1u;
		for (size_t i{ RtpPacket::HeaderSize }; i < len; ++i)
		{
			data[i] = static_cast<uint8_t>(i);
		}

		return std::unique_ptr<RtpPacket>(RtpPacket::Parse(data, len, capacity));
	};

	std::array<uint8_t, 21u> payloadStorage{};
	auto payloadPacket = makePacket(payloadStorage.data(), 20u, payloadStorage.size());
	REQUIRE(payloadPacket);
	REQUIRE(payloadPacket->SetPayloadLength(9u));
	CHECK(payloadPacket->GetSize() == payloadStorage.size());

	std::array<uint8_t, 21u> shiftStorage{};
	auto shiftPacket = makePacket(shiftStorage.data(), 20u, shiftStorage.size());
	REQUIRE(shiftPacket);
	REQUIRE(shiftPacket->ShiftPayload(2u, 1u, true));
	CHECK(shiftPacket->GetPayloadLength() == 9u);
	CHECK(shiftPacket->GetSize() == shiftStorage.size());

	std::array<uint8_t, 22u> rtxStorage{};
	auto rtxPacket = makePacket(rtxStorage.data(), 20u, rtxStorage.size());
	REQUIRE(rtxPacket);
	REQUIRE(rtxPacket->RtxEncode(97u, 22u, 33u));
	CHECK(rtxPacket->GetPayloadLength() == 10u);
	CHECK(rtxPacket->GetSize() == rtxStorage.size());

	std::array<uint8_t, 21u> shortRtxStorage{};
	auto shortRtxPacket = makePacket(shortRtxStorage.data(), 20u, shortRtxStorage.size());
	REQUIRE(shortRtxPacket);
	const auto shortRtxBefore = shortRtxStorage;
	CHECK_FALSE(shortRtxPacket->RtxEncode(97u, 22u, 33u));
	CHECK(shortRtxStorage == shortRtxBefore);
}

TEST_CASE("RtpPacket growth can consume existing RTP padding exactly", "[rtp][capacity][padding]")
{
	std::array<uint8_t, 24u> storage{};
	storage[0]  = 0xa0u;
	storage[1]  = 96u;
	storage[11] = 1u;
	for (size_t i{ RtpPacket::HeaderSize }; i < 20u; ++i)
	{
		storage[i] = static_cast<uint8_t>(i);
	}
	storage[23] = 4u;

	std::unique_ptr<RtpPacket> packet(RtpPacket::Parse(storage.data(), storage.size(), storage.size()));
	REQUIRE(packet);
	REQUIRE(packet->GetPayloadLength() == 8u);
	REQUIRE(packet->GetPayloadPadding() == 4u);
	REQUIRE(packet->ShiftPayload(2u, 4u, true));
	CHECK(packet->GetPayloadLength() == 12u);
	CHECK(packet->GetPayloadPadding() == 0u);
	CHECK(packet->GetSize() == storage.size());
}

TEST_CASE("RtpPacket clone preserves every RTP padding byte", "[rtp][clone][memory]")
{
	std::array<uint8_t, 24u> storage{};
	storage[0]  = 0xa0u;
	storage[1]  = 96u;
	storage[11] = 1u;
	for (size_t i{ RtpPacket::HeaderSize }; i < 20u; ++i)
	{
		storage[i] = static_cast<uint8_t>(0x40u + i);
	}
	storage[20] = 0x11u;
	storage[21] = 0x22u;
	storage[22] = 0x33u;
	storage[23] = 0x04u;

	std::unique_ptr<RtpPacket> packet(RtpPacket::Parse(storage.data(), storage.size()));
	REQUIRE(packet);
	REQUIRE(packet->GetPayloadPadding() == 4u);
	std::unique_ptr<RtpPacket> clone(packet->Clone());
	REQUIRE(clone);
	CHECK(clone->GetSize() == packet->GetSize());
	CHECK(std::equal(packet->GetData(), packet->GetData() + packet->GetSize(), clone->GetData()));
}

TEST_CASE("RTP extension growth only consumes owned padding", "[rtp][extensions][capacity]")
{
	std::array<uint8_t, 96u> storage{};
	storage[0]  = 0x80u;
	storage[1]  = 96u;
	storage[11] = 1u;
	std::unique_ptr<RtpPacket> packet(
	  RtpPacket::Parse(storage.data(), RtpPacket::HeaderSize, storage.size()));
	REQUIRE(packet);

	uint8_t firstValue{ 'a' };
	uint8_t secondValue{ 'b' };
	std::vector<RtpPacket::GenericExtension> adjacent{
		RtpPacket::GenericExtension(1u, 1u, &firstValue), RtpPacket::GenericExtension(2u, 1u, &secondValue)
	};
	REQUIRE(packet->SetExtensions(1u, adjacent));
	packet->SetMidExtensionId(1u);
	const auto adjacentBytes = storage;
	CHECK_FALSE(packet->UpdateMid("xy"));
	CHECK_FALSE(packet->SetExtensionLength(15u, 1u));
	CHECK_FALSE(packet->SetExtensionLength(1u, 17u));
	CHECK(storage == adjacentBytes);

	std::array<uint8_t, 8u> reservedMid{};
	std::memcpy(reservedMid.data(), "reserved", reservedMid.size());
	std::vector<RtpPacket::GenericExtension> reserved{ RtpPacket::GenericExtension(
		1u, static_cast<uint8_t>(reservedMid.size()), reservedMid.data()) };
	REQUIRE(packet->SetExtensions(1u, reserved));
	packet->SetMidExtensionId(1u);
	REQUIRE(packet->UpdateMid("a"));
	REQUIRE(packet->UpdateMid("12345678"));
	std::string mid;
	REQUIRE(packet->ReadMid(mid));
	CHECK(mid == "12345678");

	std::vector<RtpPacket::GenericExtension> emptyTwoByte{ RtpPacket::GenericExtension(
		22u, 0u, nullptr) };
	REQUIRE(packet->SetExtensions(2u, emptyTwoByte));
	CHECK(packet->HasTwoBytesExtensions());
}

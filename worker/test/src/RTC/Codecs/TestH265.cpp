#include "common.hpp"
#include "RTC/Codecs/H265.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

using namespace RTC;

namespace
{
	uint8_t H265HeaderByte(uint8_t nalType)
	{
		return static_cast<uint8_t>((nalType & 0x3F) << 1);
	}

	std::vector<uint8_t> NalUnit(uint8_t nalType, std::vector<uint8_t> payload = {})
	{
		std::vector<uint8_t> out{ H265HeaderByte(nalType), 0x01 };
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}

	void AppendNalUnit(std::vector<uint8_t>& out, const std::vector<uint8_t>& nalu)
	{
		out.push_back(static_cast<uint8_t>((nalu.size() >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>(nalu.size() & 0xFF));
		out.insert(out.end(), nalu.begin(), nalu.end());
	}

	std::unique_ptr<Codecs::H265::PayloadDescriptor> Parse(const std::vector<uint8_t>& payload)
	{
		return std::unique_ptr<Codecs::H265::PayloadDescriptor>(
		  Codecs::H265::Parse(payload.data(), payload.size()));
	}
}

SCENARIO("parse H265 payload descriptor", "[codecs][h265]")
{
	SECTION("single IDR NAL is key frame")
	{
		const auto payload = NalUnit(19, { 0x12, 0x34 });
		auto descriptor    = Parse(payload);

		REQUIRE(descriptor);
		REQUIRE(descriptor->isKeyFrame);
		REQUIRE_FALSE(descriptor->hasVps);
		REQUIRE_FALSE(descriptor->hasSps);
		REQUIRE_FALSE(descriptor->hasPps);
	}

	SECTION("single TRAIL_N NAL is not key frame")
	{
		const auto payload = NalUnit(0, { 0x12, 0x34 });
		auto descriptor    = Parse(payload);

		REQUIRE(descriptor);
		REQUIRE_FALSE(descriptor->isKeyFrame);
	}

	SECTION("single VPS/SPS/PPS are parameter sets but not key frames")
	{
		auto vps = Parse(NalUnit(32, { 0x01 }));
		auto sps = Parse(NalUnit(33, { 0x02 }));
		auto pps = Parse(NalUnit(34, { 0x03 }));

		REQUIRE(vps);
		REQUIRE(sps);
		REQUIRE(pps);
		REQUIRE(vps->hasVps);
		REQUIRE(sps->hasSps);
		REQUIRE(pps->hasPps);
		REQUIRE_FALSE(vps->isKeyFrame);
		REQUIRE_FALSE(sps->isKeyFrame);
		REQUIRE_FALSE(pps->isKeyFrame);
	}

	SECTION("FU start with IDR type is key frame")
	{
		std::vector<uint8_t> payload{ H265HeaderByte(49), 0x01, static_cast<uint8_t>(0x80 | 19), 0xaa };
		auto descriptor = Parse(payload);

		REQUIRE(descriptor);
		REQUIRE(descriptor->isKeyFrame);
	}

	SECTION("FU middle and end packets are not key frames")
	{
		std::vector<uint8_t> middle{ H265HeaderByte(49), 0x01, 19, 0xaa };
		std::vector<uint8_t> end{ H265HeaderByte(49), 0x01, static_cast<uint8_t>(0x40 | 19), 0xaa };

		auto middleDescriptor = Parse(middle);
		auto endDescriptor    = Parse(end);

		REQUIRE(middleDescriptor);
		REQUIRE(endDescriptor);
		REQUIRE_FALSE(middleDescriptor->isKeyFrame);
		REQUIRE_FALSE(endDescriptor->isKeyFrame);
	}

	SECTION("AP containing parameter sets and IDR is key frame")
	{
		std::vector<uint8_t> payload{ H265HeaderByte(48), 0x01 };
		AppendNalUnit(payload, NalUnit(32, { 0x01 }));
		AppendNalUnit(payload, NalUnit(33, { 0x02 }));
		AppendNalUnit(payload, NalUnit(34, { 0x03 }));
		AppendNalUnit(payload, NalUnit(20, { 0x04 }));

		auto descriptor = Parse(payload);

		REQUIRE(descriptor);
		REQUIRE(descriptor->isKeyFrame);
		REQUIRE(descriptor->hasVps);
		REQUIRE(descriptor->hasSps);
		REQUIRE(descriptor->hasPps);
	}

	SECTION("malformed packets fail closed")
	{
		REQUIRE_FALSE(Parse({ static_cast<uint8_t>(H265HeaderByte(19) | 0x80), 0x01, 0xaa }));
		REQUIRE_FALSE(Parse({ H265HeaderByte(19), 0x00, 0xaa }));
		REQUIRE_FALSE(Parse({ H265HeaderByte(49), 0x01 }));
		REQUIRE_FALSE(Parse({ H265HeaderByte(49), 0x01, static_cast<uint8_t>(0xC0 | 19), 0xaa }));
		REQUIRE_FALSE(Parse({ H265HeaderByte(50), 0x01, 0x00, 0x00 }));
		REQUIRE_FALSE(Parse({ H265HeaderByte(48), 0x01, 0x00, 0x04, H265HeaderByte(19), 0x01 }));
		REQUIRE_FALSE(Parse({ H265HeaderByte(48), 0x01, 0x00, 0x01, H265HeaderByte(19) }));

		std::vector<uint8_t> apWithInvalidNal{ H265HeaderByte(48), 0x01 };
		AppendNalUnit(apWithInvalidNal, { H265HeaderByte(19), 0x00, 0xaa });
		REQUIRE_FALSE(Parse(apWithInvalidNal));

		std::vector<uint8_t> apWithNestedFu{ H265HeaderByte(48), 0x01 };
		AppendNalUnit(apWithNestedFu, NalUnit(49, { 0xaa }));
		REQUIRE_FALSE(Parse(apWithNestedFu));
	}
}

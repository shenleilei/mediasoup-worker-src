#include "RTC/Codecs/FuzzerH265.hpp"
#include "RTC/Codecs/H265.hpp"

void Fuzzer::RTC::Codecs::H265::Fuzz(const uint8_t* data, size_t len)
{
	::RTC::Codecs::H265::PayloadDescriptor* descriptor = ::RTC::Codecs::H265::Parse(data, len);

	if (!descriptor)
	{
		return;
	}

	delete descriptor;
}

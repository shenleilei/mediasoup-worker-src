#define MS_CLASS "Shared"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/Shared.hpp"
#include "Logger.hpp"
#include "RTC/ProbeEgressAdapter.hpp"

namespace RTC
{
	Shared::Shared(
	  ChannelMessageRegistrator* channelMessageRegistrator,
	  Channel::ChannelNotifier* channelNotifier,
	  ProbeEgressAdapter* probeEgressAdapter)
	  : channelMessageRegistrator(channelMessageRegistrator),
	    channelNotifier(channelNotifier),
	    probeEgressAdapter(probeEgressAdapter)
	{
		MS_TRACE();
	}

	Shared::~Shared()
	{
		MS_TRACE();

		delete this->channelMessageRegistrator;
		delete this->channelNotifier;
		delete this->probeEgressAdapter;
	}
} // namespace RTC

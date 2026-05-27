#ifndef MS_RTC_SHARED_HPP
#define MS_RTC_SHARED_HPP

#include "ChannelMessageRegistrator.hpp"
#include "Channel/ChannelNotifier.hpp"

namespace RTC
{
	class ProbeEgressAdapter;

	class Shared
	{
	public:
		explicit Shared(
		  ChannelMessageRegistrator* channelMessageRegistrator,
		  Channel::ChannelNotifier* channelNotifier,
		  ProbeEgressAdapter* probeEgressAdapter = nullptr);
		~Shared();

	public:
		ChannelMessageRegistrator* channelMessageRegistrator{ nullptr };
		Channel::ChannelNotifier* channelNotifier{ nullptr };
		ProbeEgressAdapter* probeEgressAdapter{ nullptr };
	};
} // namespace RTC

#endif

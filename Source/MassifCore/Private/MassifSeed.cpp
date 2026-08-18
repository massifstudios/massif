#include "MassifSeed.h"

uint32 FMassifSeed::MakeChannelSeed(const TCHAR* ChannelName) const
{
	const uint32 ChannelHash = FCrc::StrCrc32(ChannelName);
	return HashCombine(MasterSeed, ChannelHash);
}

FRandomStream FMassifSeed::MakeChannelStream(const TCHAR* ChannelName) const
{
	return FRandomStream(static_cast<int32>(MakeChannelSeed(ChannelName)));
}

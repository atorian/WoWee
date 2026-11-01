#pragma once

#include <cstdint>

namespace wowee {
namespace auth {

// The LOGON_CHALLENGE timezone field as Wow.exe 12340 fills it (0x86D450):
// -Bias from GetTimeZoneInformation, the standard-time offset in minutes east
// of UTC. Negative offsets go on the wire as their two's complement.

// From two UTC offsets in seconds, one in January and one in July: daylight
// saving only moves a clock east, so the smaller is the standard offset.
uint32_t standardBiasMinutes(long januaryOffsetSeconds, long julyOffsetSeconds);

// This machine's standard offset, as the original client would send it.
uint32_t systemTimezoneBiasMinutes();

} // namespace auth
} // namespace wowee

#include "auth/client_timezone.hpp"
#include "core/local_time.hpp"

#include <algorithm>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wowee {
namespace auth {

uint32_t standardBiasMinutes(long januaryOffsetSeconds, long julyOffsetSeconds) {
    return static_cast<uint32_t>(std::min(januaryOffsetSeconds, julyOffsetSeconds) / 60);
}

uint32_t systemTimezoneBiasMinutes() {
#ifdef _WIN32
    TIME_ZONE_INFORMATION info{};
    if (GetTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) return 0;
    return static_cast<uint32_t>(-info.Bias);
#else
    auto offsetOn = [](int month) {
        std::tm date = core::localTime(std::time(nullptr));
        date.tm_mon = month;
        date.tm_mday = 1;
        date.tm_hour = 12;
        date.tm_min = date.tm_sec = 0;
        date.tm_isdst = -1;
        return core::localTime(std::mktime(&date)).tm_gmtoff;
    };
    return standardBiasMinutes(offsetOn(0), offsetOn(6));
#endif
}

} // namespace auth
} // namespace wowee

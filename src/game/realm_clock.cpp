#include "game/realm_clock.hpp"

namespace wowee::game {

void RealmClock::setUnixTime(uint32_t unixSeconds, Clock::time_point at) {
    anchorSeconds_ = static_cast<double>(unixSeconds);
    anchorAt_ = at;
    source_ = Source::UnixTime;
}

bool RealmClock::setPackedDate(const WowDate& date, Clock::time_point at) {
    if (source_ == Source::UnixTime) return false;
    const auto seconds = packedDateSeconds(date);
    if (!seconds) return false;
    anchorSeconds_ = *seconds;
    anchorAt_ = at;
    source_ = Source::PackedDate;
    return true;
}

std::optional<double> RealmClock::seconds(Clock::time_point at) const {
    if (source_ == Source::None) return std::nullopt;
    return anchorSeconds_ + std::chrono::duration<double>(at - anchorAt_).count();
}

std::optional<double> RealmClock::packedDateSeconds(const WowDate& date) {
    // Year 0 of the packed field is 2000, and a zero word unpacks to its first
    // minute: that is the server saying nothing, not the turn of the century.
    if (date.yearSince2000 == 0) return std::nullopt;
    if (date.hour < 0 || date.hour > 23 || date.minute < 0 || date.minute > 59) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const year_month_day ymd{year{date.fullYear()}, month{static_cast<unsigned>(date.month)},
                             day{static_cast<unsigned>(date.day)}};
    if (!ymd.ok()) return std::nullopt;
    const sys_days days{ymd};
    const double daySeconds = static_cast<double>(days.time_since_epoch().count()) * 86400.0;
    return daySeconds + date.hour * 3600.0 + date.minute * 60.0;
}

}  // namespace wowee::game

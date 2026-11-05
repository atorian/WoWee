#pragma once

namespace wowee::calendar {

/// A decoded calendar date shared by the calendar, realm clock and interface.
struct WowDate {
    int minute = 0;         // 0-59
    int hour = 0;           // 0-23
    int weekday = 0;        // 0-6, Sunday first
    int day = 1;            // 1-31
    int month = 1;          // 1-12
    int yearSince2000 = 0;  // 0-255 - add 2000 for a full year

    /// The four-digit year. FrameXML wants the short form instead: SHORTDATE
    /// formats it "%02d", so GetAchievementInfo returns yearSince2000.
    [[nodiscard]] constexpr int fullYear() const { return yearSince2000 + 2000; }
};

} // namespace wowee::calendar

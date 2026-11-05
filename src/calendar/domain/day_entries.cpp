#include "calendar/domain/records.hpp"

#include "foundation/local_time.hpp"

#include <algorithm>
#include <ctime>

namespace wowee::calendar {

namespace {

/// A day number that can be compared and subtracted across month ends, so
/// "does this holiday still run on the 3rd" does not need a calendar walk.
/// Any consistent epoch will do - only differences are used.
int64_t dayNumber(int year, int month, int day) {
    // Howard Hinnant's days_from_civil, which is exact for the proleptic
    // Gregorian calendar the server's clock uses.
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

}  // namespace

std::vector<CalendarDayEntry> calendarEntriesForDay(const CalendarData& data,
                                                    int month, int day,
                                                    int year) {
    std::vector<CalendarDayEntry> out;
    const int64_t wanted = dayNumber(year, month, day);

    for (size_t h = 0; h < data.holidays.size(); ++h) {
        const CalendarHoliday& holiday = data.holidays[h];
        for (int i = 0; i < CalendarHoliday::kNumDates; ++i) {
            if (!holiday.dates[i]) continue;
            const WowDate& start = *holiday.dates[i];
            const int64_t first = dayNumber(start.fullYear(), start.month, start.day);
            if (wanted < first) continue;
            // Hours, rounded up to whole days and never less than one: a
            // holiday sent with a duration of zero still happens on its day.
            const uint32_t hours = (i < CalendarHoliday::kNumDurations)
                                       ? holiday.durations[i] : 0;
            const int64_t days = hours > 0 ? static_cast<int64_t>((hours + 23) / 24) : 1;
            if (wanted >= first + days) continue;
            CalendarDayEntry entry;
            entry.kind = CalendarEntryKind::Holiday;
            entry.index = h;
            entry.ongoing = wanted > first;
            out.push_back(entry);
            break;  // one row per holiday per day, whichever run covers it
        }
    }

    // Raid lockouts, which the packet dates only by how long they have left.
    //
    // serverTimeUnix is the server's own clock at the moment it sent the
    // calendar, and every lockout counts down from there - so the expiry is
    // that instant plus the remainder, and it is converted here rather than at
    // each reader so the day it lands on and the time it shows cannot
    // disagree.
    if (data.serverTimeUnix != 0) {
        for (size_t l = 0; l < data.lockouts.size(); ++l) {
            const std::time_t expiry =
                static_cast<std::time_t>(data.serverTimeUnix) +
                static_cast<std::time_t>(data.lockouts[l].secondsRemaining);
            std::tm tmv{};
            tmv = core::localTime(expiry);
            if (dayNumber(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday) != wanted) {
                continue;
            }
            CalendarDayEntry entry;
            entry.kind = CalendarEntryKind::RaidLockout;
            entry.index = l;
            entry.hour = tmv.tm_hour;
            entry.minute = tmv.tm_min;
            out.push_back(entry);
        }
    }

    for (size_t e = 0; e < data.events.size(); ++e) {
        const WowDate when = data.events[e].eventTime;
        if (when.month != month || when.day != day || when.fullYear() != year) {
            continue;
        }
        CalendarDayEntry entry;
        entry.kind = CalendarEntryKind::Event;
        entry.index = e;
        out.push_back(entry);
    }

    // Events by time of day, holidays left where they are at the front.
    std::stable_sort(out.begin(), out.end(),
                     [&data](const CalendarDayEntry& a, const CalendarDayEntry& b) {
                         if (a.kind != b.kind) {
                             // The enum is in drawing order: holidays head the
                             // day, then raid lockouts, then the player's own
                             // events.
                             return a.kind < b.kind;
                         }
                         if (a.kind != CalendarEntryKind::Event) return false;
                         const WowDate& ta = data.events[a.index].eventTime;
                         const WowDate& tb = data.events[b.index].eventTime;
                         if (ta.hour != tb.hour) return ta.hour < tb.hour;
                         return ta.minute < tb.minute;
                     });
    return out;
}

} // namespace wowee::calendar

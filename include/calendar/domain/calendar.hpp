#pragma once

#include "calendar/domain/events.hpp"
#include "calendar/domain/records.hpp"

#include <vector>

namespace wowee::calendar {

/// The calendar as the server last sent it, the event last opened, and how
/// many invitations are waiting on an answer.
class Calendar {
public:
    std::vector<CalendarChange> calendarReceived(CalendarData data);
    std::vector<CalendarChange> eventReceived(CalendarEventDetail detail);
    std::vector<CalendarChange> pendingCountReceived(uint32_t count);
    std::vector<CalendarChange> inviteAlerted(std::string title);
    /// A character is logging in: the last one's calendar, its open event and
    /// its waiting invitations are not this one's.
    void beginCharacter() { *this = Calendar{}; }

    [[nodiscard]] const CalendarData& data() const { return data_; }
    [[nodiscard]] const CalendarEventDetail& detail() const { return detail_; }
    [[nodiscard]] uint32_t pendingInvites() const { return pending_; }

private:
    CalendarData data_;
    CalendarEventDetail detail_;
    uint32_t pending_ = 0;
};

} // namespace wowee::calendar

#include "calendar/domain/calendar.hpp"

#include <utility>

namespace wowee::calendar {

namespace {
/// The count the server keeps is a byte's worth.
constexpr uint32_t kMostPending = 255;
} // namespace

std::vector<CalendarChange> Calendar::calendarReceived(CalendarData data) {
    data_ = std::move(data);
    return {CalendarListUpdated{}};
}

std::vector<CalendarChange> Calendar::eventReceived(CalendarEventDetail detail) {
    detail_ = std::move(detail);
    return {EventDetailUpdated{}};
}

std::vector<CalendarChange> Calendar::pendingCountReceived(uint32_t count) {
    std::vector<CalendarChange> changes;
    // Only on a change: gametime.lua otherwise asks just on entering the world.
    if (pending_ != count) changes.emplace_back(PendingInvitesChanged{});
    pending_ = count;
    if (count > 0) changes.emplace_back(PendingInvitesWaiting{count});
    return changes;
}

std::vector<CalendarChange> Calendar::inviteAlerted(std::string title) {
    std::vector<CalendarChange> changes{InviteAlerted{std::move(title)}};
    if (pending_ < kMostPending) ++pending_;
    // The path that matters most: an invite arriving mid-session is exactly
    // when the button has to light up.
    changes.emplace_back(PendingInvitesChanged{});
    return changes;
}

} // namespace wowee::calendar

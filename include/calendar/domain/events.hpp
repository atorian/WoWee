#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::calendar {

// What happened to the calendar. Calendar returns the CalendarChange ones; the
// service publishes them and the server's notices, and the chat log and the
// addon layer subscribe.

/// The whole calendar arrived (CALENDAR_UPDATE_EVENT_LIST): the addon reads it
/// all on this and nothing else announces it.
struct CalendarListUpdated {};
/// One event in full arrived (CALENDAR_UPDATE_EVENT).
struct EventDetailUpdated {};
/// The count of unanswered invites changed (CALENDAR_UPDATE_PENDING_INVITES):
/// the minimap's date button lights its indicator from it.
struct PendingInvitesChanged {};
/// Invites are waiting, said when the server states the count.
struct PendingInvitesWaiting { uint32_t count = 0; };
/// A new invitation arrived mid-session.
struct InviteAlerted { std::string title; };

using CalendarChange = std::variant<CalendarListUpdated, EventDetailUpdated, PendingInvitesChanged,
                                    PendingInvitesWaiting, InviteAlerted>;

/// Why the server refused a calendar command (CalendarError).
enum class CommandRefusal {
    Internal,
    GuildEventLimit,
    EventLimit,
    CannotInvite,
    NoInvitesLeft,
    InvalidDate,
    CannotInviteSelf,
    CannotModify,
    NotInvited,
    AlreadyInvited,
    PlayerNotFound,
    NotEnoughFocus,
    EventLocked,
    EventDeleted,
    NotModerator,
    Other,
};

struct CommandRefused { CommandRefusal reason = CommandRefusal::Other; };
/// The player's own answer to an invitation changed. `status` is a
/// CalendarInviteStatus.
struct InviteStatusChanged { uint8_t status = 0; std::string title; };
struct LockoutAdded { uint32_t mapId = 0; uint32_t difficulty = 0; };
struct LockoutRemoved { uint32_t mapId = 0; uint32_t difficulty = 0; };

} // namespace wowee::calendar

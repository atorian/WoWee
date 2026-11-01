#pragma once

#include "calendar/domain/events.hpp"

#include <cstdint>
#include <string>

namespace wowee::calendar {

struct CalendarData;
struct CalendarEventDetail;
struct CalendarEventDraft;

/// The calendar as the interface and the addon layer use it.
class ICalendar {
public:
    virtual ~ICalendar() = default;

    // ---- Reading ----
    [[nodiscard]] virtual uint32_t getCalendarPendingInvites() const = 0;
    /// The calendar the server last sent, empty until it has answered one.
    [[nodiscard]] virtual const CalendarData& getCalendarData() const = 0;
    /// The event last opened, in full.
    [[nodiscard]] virtual const CalendarEventDetail& getCalendarEventDetail() const = 0;

    // ---- Requests ----
    /// The calendar, and the count of invites waiting on an answer.
    virtual void requestCalendar() = 0;
    /// Ask for one event's detail: CMSG_CALENDAR_GET_EVENT.
    virtual void requestCalendarEvent(uint64_t eventId) = 0;
    /// Create an event: CMSG_CALENDAR_ADD_EVENT.
    virtual void createCalendarEvent(const CalendarEventDraft& draft) = 0;
    /// Edit an existing event: CMSG_CALENDAR_UPDATE_EVENT.
    virtual void updateCalendarEvent(uint64_t eventId, uint64_t inviteId, const CalendarEventDraft& draft) = 0;
    /// Delete one: CMSG_CALENDAR_REMOVE_EVENT.
    virtual void removeCalendarEvent(uint64_t eventId, uint64_t inviteId) = 0;
    /// Invite someone: CMSG_CALENDAR_EVENT_INVITE. A pre-invite belongs to an
    /// event that has not been created yet, and carries no event id.
    virtual void inviteToCalendarEvent(uint64_t eventId, uint64_t inviteId, const std::string& name,
                                       bool isPreInvite, bool isGuildEvent) = 0;
    /// Invite the guild by filter: CMSG_CALENDAR_GUILD_FILTER.
    virtual void massInviteGuildToCalendarEvent(uint32_t minLevel, uint32_t maxLevel, uint32_t minRank) = 0;
    /// Set another invitee's status, or their moderator rank. Both act on
    /// someone else; the player's own answer is respondToCalendarInvite.
    virtual void setCalendarInviteStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId,
                                         uint8_t status) = 0;
    virtual void setCalendarInviteModerator(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId,
                                            uint8_t rank) = 0;
    /// Answer an invitation: CMSG_CALENDAR_EVENT_RSVP with a
    /// CalendarInviteStatus (1 accepted, 2 declined, 8 tentative, 9 removed).
    virtual void respondToCalendarInvite(uint64_t eventId, uint64_t inviteId, uint32_t status) = 0;
};

/// What the protocol layer tells the calendar.
class ICalendarFeed {
public:
    virtual ~ICalendarFeed() = default;

    virtual void onCalendar(CalendarData data) = 0;
    virtual void onEvent(CalendarEventDetail detail) = 0;
    virtual void onPendingCount(uint32_t count) = 0;
    virtual void onInviteAlert(std::string title) = 0;
    /// SMSG_CALENDAR_COMMAND_RESULT, refused; the server says nothing more
    /// when a command goes through.
    virtual void onCommandRefused(CommandRefusal reason) = 0;
    /// The player's own answer to an invitation changed.
    virtual void onInviteStatus(uint8_t status, std::string title) = 0;
    virtual void onLockoutAdded(uint32_t mapId, uint32_t difficulty) = 0;
    virtual void onLockoutRemoved(uint32_t mapId, uint32_t difficulty) = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::calendar

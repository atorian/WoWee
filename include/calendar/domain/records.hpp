#pragma once

#include "core/packed_time.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::calendar {

using core::WowDate;

// The calendar's records: what the server sends of it, an event in full, and
// the draft an event is built in. The protocol reads them
// (game/calendar_data.hpp) under its own names for them.

/// An invitation waiting on an answer.
struct CalendarInvite {
    uint64_t eventId = 0;
    uint64_t inviteId = 0;
    uint8_t status = 0;        ///< CalendarInviteStatus
    uint8_t rank = 0;          ///< CalendarModerationRank
    bool isGuildEvent = false;
    uint64_t creatorGuid = 0;  ///< the sender's, when the event itself is gone
};

/// An event the player can see, whether or not they have answered it.
struct CalendarEvent {
    uint64_t eventId = 0;
    std::string title;
    uint32_t type = 0;         ///< CalendarEventType
    WowDate eventTime;
    uint32_t eventTimePacked = 0;  ///< kept raw: the interface sorts on it
    uint32_t flags = 0;
    int32_t dungeonId = 0;
    uint64_t creatorGuid = 0;
};

/// A raid the player is saved to, and how long that lasts.
struct CalendarRaidLockout {
    uint32_t mapId = 0;
    uint32_t difficulty = 0;
    uint32_t secondsRemaining = 0;
    uint64_t instanceGuid = 0;
};

/// When a raid's lock resets, and how often.
struct CalendarRaidReset {
    int32_t mapId = 0;
    int32_t periodSeconds = 0;
    int32_t offsetSeconds = 0;
};

/// A world holiday, with the dates it runs on.
struct CalendarHoliday {
    static constexpr int kNumDates = 26;      ///< server MAX_HOLIDAY_DATES
    static constexpr int kNumDurations = 10;  ///< server MAX_HOLIDAY_DURATIONS
    static constexpr int kNumFlags = 10;      ///< server MAX_HOLIDAY_FLAGS

    uint32_t id = 0;
    uint32_t region = 0;
    uint32_t looping = 0;
    uint32_t priority = 0;
    uint32_t calendarFilterType = 0;
    uint32_t dates[kNumDates] = {};
    uint32_t durations[kNumDurations] = {};
    uint32_t calendarFlags[kNumFlags] = {};
    std::string textureFilename;
};

/// Everything the packet carried.
struct CalendarData {
    std::vector<CalendarInvite> invites;
    std::vector<CalendarEvent> events;
    /// The server's own clock, so a client whose clock is wrong still places
    /// events on the right day.
    uint32_t serverTimeUnix = 0;
    WowDate zoneTime;
    std::vector<CalendarRaidLockout> lockouts;
    uint32_t resetRelationTime = 0;
    std::vector<CalendarRaidReset> resets;
    std::vector<CalendarHoliday> holidays;
};

/// One person on an event's invite list.
struct CalendarEventInvitee {
    uint64_t guid = 0;
    uint64_t inviteId = 0;
    uint8_t level = 0;
    uint8_t status = 0;        ///< CalendarInviteStatus
    uint8_t rank = 0;          ///< CalendarModerationRank
    bool isGuildMember = false;
    WowDate statusTime;
    std::string note;
};

/**
 * One event in full, as SMSG_CALENDAR_SEND_EVENT sends it.
 *
 * The answer to opening an event. Laid out against CalendarMgr::
 * SendCalendarEvent (CalendarMgr.cpp:627): a header, then a count and that
 * many invitee rows - and every row carries a packed guid, a packed time and
 * a string, so a row read wrong slides the rest exactly as the calendar packet
 * does.
 */
struct CalendarEventDetail {
    uint8_t sendType = 0;
    uint64_t creatorGuid = 0;
    uint64_t eventId = 0;
    std::string title;
    std::string description;
    uint8_t type = 0;
    uint8_t repeatOption = 0;
    uint32_t maxInvites = 0;
    int32_t dungeonId = -1;
    uint32_t flags = 0;
    WowDate eventTime;
    uint32_t eventTimePacked = 0;
    WowDate timeZoneTime;
    uint32_t guildId = 0;
    std::vector<CalendarEventInvitee> invitees;
};

/// An event being built up before it is sent.
///
/// The interface fills this in over several calls - a title here, a date
/// there - and commits it with one, so it has to be held somewhere between
/// them. That is how WoW works too: CalendarNewEvent starts one,
/// CalendarEventSetTitle and friends fill it, CalendarAddEvent sends it.
struct CalendarEventDraft {
    std::string title;
    std::string description;
    uint8_t type = 0;            ///< CalendarEventType
    uint8_t repeatOption = 0;    ///< CalendarRepeatType: 0 never, 1 weekly, 2 fortnightly
    uint32_t maxInvites = 100;
    int32_t dungeonId = -1;      ///< -1 for no dungeon, and signed for that reason
    WowDate eventTime;
    uint32_t flags = 0;
};

/// What a row on a given day came from. The interface reads a day's rows
/// through one index space - `CalendarGetNumDayEvents` counts them and
/// `CalendarGetDayEvent`, `CalendarGetHolidayInfo` and `CalendarGetRaidInfo`
/// all index into the same list - so the order has to be decided once, here,
/// rather than in each of those.
enum class CalendarEntryKind { Holiday, RaidLockout, Event };

struct CalendarDayEntry {
    CalendarEntryKind kind = CalendarEntryKind::Event;
    /// Into CalendarData::holidays, ::lockouts or ::events, by kind.
    size_t index = 0;
    /// For a raid lockout, the hour and minute it expires. The packet gives
    /// seconds remaining rather than a time, so it is worked out once here
    /// with the rest of the day's placement rather than at each reader.
    int hour = 0;
    int minute = 0;
    /// True on every day of a holiday after the first. FrameXML skips a row
    /// whose sequenceType is "ONGOING" when drawing the day's list, so a
    /// week-long holiday draws its name once rather than seven times.
    bool ongoing = false;
};

/**
 * The rows for one day, holidays first and then events by time.
 *
 * Holidays lead because that is the order WoW draws them in - a world event
 * heads the day and the player's own events follow underneath.
 *
 * A holiday covers `durations[i]` hours from `dates[i]`, and both arrays are
 * walked together: a holiday with several date/duration pairs is several
 * separate runs in the same row, which is how a recurring one is sent.
 */
std::vector<CalendarDayEntry> calendarEntriesForDay(const CalendarData& data,
                                                    int month, int day,
                                                    int year);

} // namespace wowee::calendar

#pragma once

#include <cstdint>

namespace wowee {
namespace network { class Packet; }
namespace calendar {
struct CalendarInvite;
struct CalendarEvent;
struct CalendarRaidLockout;
struct CalendarRaidReset;
struct CalendarHoliday;
struct CalendarData;
struct CalendarEventInvitee;
struct CalendarEventDetail;
struct CalendarEventDraft;
}
namespace game {

// The calendar's records are the calendar domain's
// (calendar/domain/records.hpp); these are their protocol names. Their
// definitions are included where they are used.
using CalendarInvite = calendar::CalendarInvite;
using CalendarEvent = calendar::CalendarEvent;
using CalendarRaidLockout = calendar::CalendarRaidLockout;
using CalendarRaidReset = calendar::CalendarRaidReset;
using CalendarHoliday = calendar::CalendarHoliday;
using CalendarData = calendar::CalendarData;
using CalendarEventInvitee = calendar::CalendarEventInvitee;
using CalendarEventDetail = calendar::CalendarEventDetail;
using CalendarEventDraft = calendar::CalendarEventDraft;

/**
 * The calendar the server sends in one packet, unpacked.
 *
 * SMSG_CALENDAR_SEND_CALENDAR is the answer to CMSG_CALENDAR_GET_CALENDAR and
 * carries six lists back to back, each a count followed by its rows. Nothing
 * in it is length-prefixed as a whole and four of the six rows contain a
 * packed guid or a string, so a row read wrong does not fail - it slides
 * everything after it, and the next count is read out of the middle of a
 * string. That is why this is a parser with a test rather than a read done
 * inline: the failure mode is silent and total.
 *
 * Layout, from AzerothCore's WorldSession::HandleCalendarGetCalendar
 * (CalendarHandler.cpp:53) which is the writer this parses:
 *
 *     uint32 numInvites
 *       uint64 eventId, uint64 inviteId, uint8 status, uint8 rank,
 *       uint8 isGuildEvent, packedGuid creator
 *     uint32 numEvents
 *       uint64 eventId, cstring title, uint32 type, packedTime eventTime,
 *       uint32 flags, int32 dungeonId, packedGuid creator
 *     uint32 serverTime, packedTime zoneTime
 *     uint32 numLockouts
 *       uint32 mapId, uint32 difficulty, uint32 secondsLeft, uint64 instanceGuid
 *     uint32 resetRelationTime
 *     uint32 numResets
 *       int32 mapId, int32 period, int32 offset
 *     uint32 numHolidays
 *       uint32 id, region, looping, priority, calendarFilterType,
 *       uint32 date[26], uint32 duration[10], uint32 calendarFlags[10],
 *       cstring textureFilename
 *
 * The holiday row is 49 uint32s and a string; the three array lengths are the
 * server's MAX_HOLIDAY_DATES, MAX_HOLIDAY_DURATIONS and MAX_HOLIDAY_FLAGS
 * (DBCStructure.h:1122). Getting one of those counts wrong misreads every
 * holiday after the first and nothing says so.
 */

/**
 * Read one SMSG_CALENDAR_SEND_CALENDAR.
 *
 * False when the packet runs out mid-row, and `out` is then whatever had been
 * read whole - a truncated calendar rather than a mixture of real values and
 * values read past the end. Every count is checked against what is left before
 * it is trusted: the counts are server-controlled uint32s and a corrupt one
 * would otherwise reserve four billion rows.
 */
bool parseCalendarSendCalendar(network::Packet& packet, CalendarData& out);

/// Read one SMSG_CALENDAR_SEND_EVENT. False when it runs out mid-row.
bool parseCalendarSendEvent(network::Packet& packet, CalendarEventDetail& out);

}  // namespace game
}  // namespace wowee

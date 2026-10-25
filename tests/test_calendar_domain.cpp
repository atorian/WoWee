// The calendar, end to end: the server's calendar packets, what the interface
// reads and is told, the lines the player sees, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "calendar/domain/records.hpp"
#include "calendar/services/calendar.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kEventId = 0x5001;
constexpr uint64_t kCreator = 0x21;

/// SMSG_CALENDAR_SEND_CALENDAR with one event and every other list empty.
std::vector<uint8_t> calendarWithEvent(const std::string& title) {
    return Body()
        .u32(0)                                             // invites
        .u32(1)                                             // events
        .u64(kEventId).cstr(title).u32(3 /* raid */).u32(0x1A2B3C4D).u32(0).u32(static_cast<uint32_t>(-1))
        .packedGuid(kCreator)
        .u32(1729800000).u32(0x1A2B3C4D)                    // server time, zone time
        .u32(0)                                             // lockouts
        .u32(0)                                             // reset relation time
        .u32(0)                                             // resets
        .u32(0)                                             // holidays
        .bytes();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

size_t chatCount(GameHarness& h, const std::string& text) {
    const auto& history = h.game().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(),
                                             [&](const auto& line) { return line.message == text; }));
}

}  // namespace

TEST_CASE("the calendar the server sends is what the addon reads, and a cut-short one leaves it",
          "[calendar][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_CALENDAR_SEND_CALENDAR, calendarWithEvent("Karazhan"));
    h.pump();

    const auto& data = h.calendar().getCalendarData();
    REQUIRE(data.events.size() == 1);
    CHECK(data.events[0].title == "Karazhan");
    CHECK(data.events[0].creatorGuid == kCreator);
    CHECK(h.addonEvents("CALENDAR_UPDATE_EVENT_LIST").size() == 1);

    // One event promised and not there.
    h.receive(Opcode::SMSG_CALENDAR_SEND_CALENDAR, Body().u32(0).u32(1).bytes());
    h.pump();
    REQUIRE(h.calendar().getCalendarData().events.size() == 1);
    CHECK(h.calendar().getCalendarData().events[0].title == "Karazhan");
    CHECK(h.addonEvents("CALENDAR_UPDATE_EVENT_LIST").size() == 1);
}

TEST_CASE("the waiting invites light the minimap when their count changes", "[calendar][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_CALENDAR_SEND_NUM_PENDING, Body().u32(2).bytes());
    h.receive(Opcode::SMSG_CALENDAR_SEND_NUM_PENDING, Body().u32(2).bytes());
    h.pump();

    CHECK(h.calendar().getCalendarPendingInvites() == 2);
    CHECK(h.addonEvents("CALENDAR_UPDATE_PENDING_INVITES").size() == 1);
    CHECK(chatCount(h, "You have 2 pending calendar invites.") == 2);

    // An invitation arriving mid-session adds one and lights it again.
    h.receive(Opcode::SMSG_CALENDAR_EVENT_INVITE_ALERT, Body().u64(kEventId).cstr("Karazhan").bytes());
    h.pump();
    CHECK(h.calendar().getCalendarPendingInvites() == 3);
    CHECK(h.addonEvents("CALENDAR_UPDATE_PENDING_INVITES").size() == 2);
    CHECK(chatSaid(h, "Calendar invite: Karazhan"));
}

TEST_CASE("a refused command says why, and one that went through says nothing", "[calendar][harness]") {
    GameHarness h;
    h.enterWorld();
    const size_t linesBefore = h.game().getChatHistory().size();

    h.receive(Opcode::SMSG_CALENDAR_COMMAND_RESULT, Body().u32(7).u8(0).cstr("").bytes());
    h.pump();
    CHECK(h.game().getChatHistory().size() == linesBefore);

    h.receive(Opcode::SMSG_CALENDAR_COMMAND_RESULT, Body().u32(7).u8(7).cstr("").bytes());
    h.receive(Opcode::SMSG_CALENDAR_COMMAND_RESULT, Body().u32(7).u8(40).cstr("").bytes());
    h.pump();
    CHECK(chatSaid(h, "Calendar: Cannot invite yourself."));
    CHECK(chatSaid(h, "Calendar: Command failed."));
}

TEST_CASE("the player's own answer and a raid lockout are said in the chat", "[calendar][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_CALENDAR_EVENT_STATUS,
              Body().u64(1).u64(kEventId).u8(3).u32(0).u64(0).u8(8 /* tentative */).u8(0).u8(0)
                  .cstr("Karazhan").bytes());
    h.receive(Opcode::SMSG_CALENDAR_RAID_LOCKOUT_ADDED, Body().u64(0).u64(0).u32(533).u32(1).u64(0).bytes());
    h.pump();

    CHECK(chatSaid(h, "Calendar event 'Karazhan': your RSVP is tentative."));
    CHECK(chatSaid(h, "Calendar: Raid lockout added for map #533 (Heroic)."));
}

TEST_CASE("opening the calendar asks for it and for the count; an answer names the event",
          "[calendar][harness]") {
    GameHarness h;
    h.enterWorld();

    h.calendar().requestCalendar();
    CHECK(h.lastSent(Opcode::CMSG_CALENDAR_GET_CALENDAR));
    CHECK(h.lastSent(Opcode::CMSG_CALENDAR_GET_NUM_PENDING));

    h.calendar().respondToCalendarInvite(kEventId, 9, 1 /* accepted */);
    auto* rsvp = h.lastSent(Opcode::CMSG_CALENDAR_EVENT_RSVP);
    REQUIRE(rsvp);
    CHECK(rsvp->readUInt64() == kEventId);
    CHECK(rsvp->readUInt64() == 9);
    CHECK(rsvp->readUInt32() == 1);
}

TEST_CASE("the next character has none of the last one's calendar", "[calendar][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_CALENDAR_SEND_CALENDAR, calendarWithEvent("Karazhan"));
    h.receive(Opcode::SMSG_CALENDAR_SEND_NUM_PENDING, Body().u32(2).bytes());
    h.pump();
    REQUIRE(h.calendar().getCalendarPendingInvites() == 2);

    h.relogAs(bob);

    CHECK(h.calendar().getCalendarData().events.empty());
    CHECK(h.calendar().getCalendarPendingInvites() == 0);
}

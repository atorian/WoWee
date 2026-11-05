// The calendar: what it holds and when it announces the invites waiting, and
// the service against a recording gateway and the real event bus. Only
// wowee_calendar is linked.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "calendar/services/calendar_service.hpp"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::calendar;

namespace {

template <typename E>
int count(const std::vector<CalendarChange>& changes) {
    return static_cast<int>(std::count_if(changes.begin(), changes.end(),
                                          [](const CalendarChange& c) { return std::holds_alternative<E>(c); }));
}

struct RecordingGateway final : ICalendarGateway {
    bool world = true;
    std::vector<std::string> sent;

    bool online() const override { return world; }
    void sendCalendarRequest() override { sent.push_back("calendar"); }
    void sendPendingCountRequest() override { sent.push_back("pending"); }
    void sendEventRequest(uint64_t id) override { sent.push_back("event " + std::to_string(id)); }
    void sendAdd(const CalendarEventDraft& d) override { sent.push_back("add " + d.title); }
    void sendUpdate(uint64_t id, uint64_t, const CalendarEventDraft& d) override { sent.push_back("update " + std::to_string(id) + " " + d.title); }
    void sendRemove(uint64_t id, uint64_t) override { sent.push_back("remove " + std::to_string(id)); }
    void sendInvite(uint64_t, uint64_t, const std::string& name, bool, bool) override { sent.push_back("invite " + name); }
    void sendGuildFilter(uint32_t, uint32_t, uint32_t) override { sent.push_back("guild"); }
    void sendInviteStatus(uint64_t, uint64_t, uint64_t, uint8_t s) override { sent.push_back("status " + std::to_string(s)); }
    void sendModeratorStatus(uint64_t, uint64_t, uint64_t, uint8_t r) override { sent.push_back("moderator " + std::to_string(r)); }
    void sendRsvp(uint64_t, uint64_t, uint32_t s) override { sent.push_back("rsvp " + std::to_string(s)); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    CalendarService service{gateway, bus};
};

} // namespace

TEST_CASE("The pending count is announced when it changes, and said while above zero", "[calendar]") {
    Calendar c;
    auto first = c.pendingCountReceived(2);
    CHECK(count<PendingInvitesChanged>(first) == 1);
    CHECK(count<PendingInvitesWaiting>(first) == 1);
    auto same = c.pendingCountReceived(2);
    CHECK(count<PendingInvitesChanged>(same) == 0);
    CHECK(count<PendingInvitesWaiting>(same) == 1);
    auto none = c.pendingCountReceived(0);
    CHECK(count<PendingInvitesChanged>(none) == 1);
    CHECK(count<PendingInvitesWaiting>(none) == 0);
    CHECK(c.pendingInvites() == 0);
}

TEST_CASE("An invite alert adds one waiting invite, up to a byte's worth", "[calendar]") {
    Calendar c;
    c.pendingCountReceived(254);
    auto changes = c.inviteAlerted("Raid night");
    CHECK(c.pendingInvites() == 255);
    CHECK(std::get<InviteAlerted>(changes.front()).title == "Raid night");
    CHECK(count<PendingInvitesChanged>(changes) == 1);
    c.inviteAlerted("");
    CHECK(c.pendingInvites() == 255);
}

TEST_CASE("The calendar and the event opened are kept as sent", "[calendar]") {
    Calendar c;
    CalendarData data;
    data.events.resize(3);
    CHECK(count<CalendarListUpdated>(c.calendarReceived(data)) == 1);
    CHECK(c.data().events.size() == 3);
    CalendarEventDetail detail;
    detail.title = "Karazhan";
    CHECK(count<EventDetailUpdated>(c.eventReceived(detail)) == 1);
    CHECK(c.detail().title == "Karazhan");
}

TEST_CASE("A new character has none of the last one's calendar", "[calendar]") {
    Calendar c;
    CalendarData data;
    data.events.resize(2);
    c.calendarReceived(data);
    CalendarEventDetail detail;
    detail.title = "Karazhan";
    c.eventReceived(detail);
    c.pendingCountReceived(3);

    c.beginCharacter();

    CHECK(c.data().events.empty());
    CHECK(c.detail().title.empty());
    CHECK(c.pendingInvites() == 0);
    // The new character's first count is a change worth announcing.
    CHECK(count<PendingInvitesChanged>(c.pendingCountReceived(3)) == 1);
}

TEST_CASE("Asking for the calendar asks for the pending count too", "[calendar]") {
    Fixture f;
    f.service.requestCalendar();
    CHECK(f.gateway.sent == std::vector<std::string>{"calendar", "pending"});
}

TEST_CASE("Nothing is sent outside the world", "[calendar]") {
    Fixture f;
    f.gateway.world = false;
    CalendarEventDraft draft;
    draft.title = "x";
    f.service.requestCalendar();
    f.service.requestCalendarEvent(1);
    f.service.createCalendarEvent(draft);
    f.service.updateCalendarEvent(1, 0, draft);
    f.service.removeCalendarEvent(1, 0);
    f.service.inviteToCalendarEvent(1, 0, "Ann", false, false);
    f.service.massInviteGuildToCalendarEvent(1, 80, 0);
    f.service.setCalendarInviteStatus(2, 1, 0, 1);
    f.service.setCalendarInviteModerator(2, 1, 0, 1);
    f.service.respondToCalendarInvite(1, 0, 1);
    CHECK(f.gateway.sent.empty());
}

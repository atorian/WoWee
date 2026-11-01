#include "calendar/services/calendar_service.hpp"

#include "core/event_bus.hpp"

namespace wowee::calendar {

CalendarService::CalendarService(ICalendarGateway& gateway, core::EventBus& events)
    : gateway_(gateway), events_(events) {}

void CalendarService::apply(const std::vector<CalendarChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void CalendarService::requestCalendar() {
    if (!gateway_.online()) return;
    gateway_.sendCalendarRequest();
    gateway_.sendPendingCountRequest();
}

void CalendarService::requestCalendarEvent(uint64_t eventId) {
    if (gateway_.online()) gateway_.sendEventRequest(eventId);
}

void CalendarService::createCalendarEvent(const CalendarEventDraft& draft) {
    if (gateway_.online()) gateway_.sendAdd(draft);
}

void CalendarService::updateCalendarEvent(uint64_t eventId, uint64_t inviteId, const CalendarEventDraft& draft) {
    if (gateway_.online()) gateway_.sendUpdate(eventId, inviteId, draft);
}

void CalendarService::removeCalendarEvent(uint64_t eventId, uint64_t inviteId) {
    if (gateway_.online()) gateway_.sendRemove(eventId, inviteId);
}

void CalendarService::inviteToCalendarEvent(uint64_t eventId, uint64_t inviteId, const std::string& name,
                                            bool isPreInvite, bool isGuildEvent) {
    if (gateway_.online()) gateway_.sendInvite(eventId, inviteId, name, isPreInvite, isGuildEvent);
}

void CalendarService::massInviteGuildToCalendarEvent(uint32_t minLevel, uint32_t maxLevel, uint32_t minRank) {
    if (gateway_.online()) gateway_.sendGuildFilter(minLevel, maxLevel, minRank);
}

void CalendarService::setCalendarInviteStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId,
                                              uint8_t status) {
    if (gateway_.online()) gateway_.sendInviteStatus(inviteeGuid, eventId, inviteId, status);
}

void CalendarService::setCalendarInviteModerator(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId,
                                                 uint8_t rank) {
    if (gateway_.online()) gateway_.sendModeratorStatus(inviteeGuid, eventId, inviteId, rank);
}

void CalendarService::respondToCalendarInvite(uint64_t eventId, uint64_t inviteId, uint32_t status) {
    if (gateway_.online()) gateway_.sendRsvp(eventId, inviteId, status);
}

void CalendarService::onCalendar(CalendarData data) { apply(calendar_.calendarReceived(std::move(data))); }
void CalendarService::onEvent(CalendarEventDetail detail) { apply(calendar_.eventReceived(std::move(detail))); }
void CalendarService::onPendingCount(uint32_t count) { apply(calendar_.pendingCountReceived(count)); }
void CalendarService::onInviteAlert(std::string title) { apply(calendar_.inviteAlerted(std::move(title))); }

void CalendarService::onCommandRefused(CommandRefusal reason) {
    events_.publish(CommandRefused{reason});
}

void CalendarService::onInviteStatus(uint8_t status, std::string title) {
    events_.publish(InviteStatusChanged{status, std::move(title)});
}

void CalendarService::onLockoutAdded(uint32_t mapId, uint32_t difficulty) {
    events_.publish(LockoutAdded{mapId, difficulty});
}

void CalendarService::onLockoutRemoved(uint32_t mapId, uint32_t difficulty) {
    events_.publish(LockoutRemoved{mapId, difficulty});
}

} // namespace wowee::calendar

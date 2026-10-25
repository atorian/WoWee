#pragma once

#include "calendar/domain/calendar.hpp"
#include "calendar/services/calendar.hpp"
#include "calendar/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::calendar {

class CalendarService final : public ICalendar, public ICalendarFeed {
public:
    CalendarService(ICalendarGateway& gateway, core::EventBus& events);

    // ICalendar: reading
    [[nodiscard]] uint32_t getCalendarPendingInvites() const override { return calendar_.pendingInvites(); }
    [[nodiscard]] const CalendarData& getCalendarData() const override { return calendar_.data(); }
    [[nodiscard]] const CalendarEventDetail& getCalendarEventDetail() const override { return calendar_.detail(); }

    // ICalendar: requests
    void requestCalendar() override;
    void requestCalendarEvent(uint64_t eventId) override;
    void createCalendarEvent(const CalendarEventDraft& draft) override;
    void updateCalendarEvent(uint64_t eventId, uint64_t inviteId, const CalendarEventDraft& draft) override;
    void removeCalendarEvent(uint64_t eventId, uint64_t inviteId) override;
    void inviteToCalendarEvent(uint64_t eventId, uint64_t inviteId, const std::string& name,
                               bool isPreInvite, bool isGuildEvent) override;
    void massInviteGuildToCalendarEvent(uint32_t minLevel, uint32_t maxLevel, uint32_t minRank) override;
    void setCalendarInviteStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t status) override;
    void setCalendarInviteModerator(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t rank) override;
    void respondToCalendarInvite(uint64_t eventId, uint64_t inviteId, uint32_t status) override;

    // ICalendarFeed
    void onCalendar(CalendarData data) override;
    void onEvent(CalendarEventDetail detail) override;
    void onPendingCount(uint32_t count) override;
    void onInviteAlert(std::string title) override;
    void onCommandRefused(CommandRefusal reason) override;
    void onInviteStatus(uint8_t status, std::string title) override;
    void onLockoutAdded(uint32_t mapId, uint32_t difficulty) override;
    void onLockoutRemoved(uint32_t mapId, uint32_t difficulty) override;
    void beginCharacter() override { calendar_.beginCharacter(); }

private:
    void apply(const std::vector<CalendarChange>& changes);

    ICalendarGateway& gateway_;
    core::EventBus& events_;
    Calendar calendar_;
};

} // namespace wowee::calendar

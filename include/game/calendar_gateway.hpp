#pragma once

#include "calendar/domain/events.hpp"
#include "calendar/services/ports.hpp"
#include "foundation/event_bus.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

class PacketSender;
class SocialQueries;
class ChatLog;
class FrameXmlEvents;

/// The calendar service's port, implemented over the world connection.
class CalendarGateway final : public calendar::ICalendarGateway {
public:
    explicit CalendarGateway(PacketSender& sender) : sender_(sender) {}

    [[nodiscard]] bool online() const override;
    void sendCalendarRequest() override;
    void sendPendingCountRequest() override;
    void sendEventRequest(uint64_t eventId) override;
    void sendAdd(const calendar::CalendarEventDraft& draft) override;
    void sendUpdate(uint64_t eventId, uint64_t inviteId, const calendar::CalendarEventDraft& draft) override;
    void sendRemove(uint64_t eventId, uint64_t inviteId) override;
    void sendInvite(uint64_t eventId, uint64_t inviteId, const std::string& name,
                    bool isPreInvite, bool isGuildEvent) override;
    void sendGuildFilter(uint32_t minLevel, uint32_t maxLevel, uint32_t minRank) override;
    void sendInviteStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t status) override;
    void sendModeratorStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t rank) override;
    void sendRsvp(uint64_t eventId, uint64_t inviteId, uint32_t status) override;

private:
    PacketSender& sender_;
};

/// SMSG_CALENDAR_COMMAND_RESULT's error, as AzerothCore's CalendarError writes it.
[[nodiscard]] calendar::CommandRefusal calendarCommandRefusal(uint8_t result);

/// What the player sees of the calendar: the CALENDAR_UPDATE_* events and the
/// chat lines. `names` names the maps of raid lockouts.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeCalendarNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, SocialQueries& names);

} // namespace wowee::game

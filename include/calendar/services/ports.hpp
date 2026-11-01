#pragma once

#include <cstdint>
#include <string>

namespace wowee::calendar {

struct CalendarEventDraft;

/// Requests to the server about the calendar. Implemented by the protocol
/// layer.
class ICalendarGateway {
public:
    virtual ~ICalendarGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;

    virtual void sendCalendarRequest() = 0;
    virtual void sendPendingCountRequest() = 0;
    virtual void sendEventRequest(uint64_t eventId) = 0;
    virtual void sendAdd(const CalendarEventDraft& draft) = 0;
    virtual void sendUpdate(uint64_t eventId, uint64_t inviteId, const CalendarEventDraft& draft) = 0;
    virtual void sendRemove(uint64_t eventId, uint64_t inviteId) = 0;
    virtual void sendInvite(uint64_t eventId, uint64_t inviteId, const std::string& name,
                            bool isPreInvite, bool isGuildEvent) = 0;
    virtual void sendGuildFilter(uint32_t minLevel, uint32_t maxLevel, uint32_t minRank) = 0;
    virtual void sendInviteStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t status) = 0;
    virtual void sendModeratorStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t rank) = 0;
    virtual void sendRsvp(uint64_t eventId, uint64_t inviteId, uint32_t status) = 0;
};

} // namespace wowee::calendar

#include "game/calendar_gateway.hpp"

#include "calendar/domain/records.hpp"
#include "core/logger.hpp"
#include "core/packed_time.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/group_defines.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "network/packet.hpp"

#include <cstdio>

namespace wowee::game {

using calendar::CommandRefusal;

bool CalendarGateway::online() const {
    return sender_.characterInWorld();
}

void CalendarGateway::sendInvite(uint64_t eventId, uint64_t inviteId, const std::string& name,
                                 bool isPreInvite, bool isGuildEvent) {
    // eventId, inviteId, name, isPreInvite, isGuildEvent - CalendarHandler.cpp:515.
    // The two flags are ByteBuffer bools, which are one byte each on the wire.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_INVITE));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeString(name);
    pkt.writeUInt8(isPreInvite ? 1 : 0);
    pkt.writeUInt8(isGuildEvent ? 1 : 0);
    sender_.send(pkt);
    LOG_INFO("inviteToCalendarEvent: '", name, "' to event ", eventId,
             isPreInvite ? " (before it is created)" : "");
}

void CalendarGateway::sendGuildFilter(uint32_t minLevel, uint32_t maxLevel, uint32_t minRank) {
    // Its own opcode, not an invite with the name left out: the server picks
    // the members itself from these three bounds and never sees a name
    // (CalendarHandler.cpp:198, and Guild::MassInviteToEvent behind it).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GUILD_FILTER));
    pkt.writeUInt32(minLevel);
    pkt.writeUInt32(maxLevel);
    pkt.writeUInt32(minRank);
    sender_.send(pkt);
    LOG_INFO("massInviteGuildToCalendarEvent: levels ", minLevel, "-", maxLevel,
             ", rank ", minRank, " and above");
}

void CalendarGateway::sendUpdate(uint64_t eventId, uint64_t inviteId, const calendar::CalendarEventDraft& draft) {
    // The same body as adding, with the event and invite ids in front of it
    // (CalendarHandler.cpp:360). Written out rather than shared with
    // sendAdd: the two are the same shape today and are two
    // different opcodes, and folding them together would make the next change
    // to one silently change the other.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_UPDATE_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeString(draft.title);
    pkt.writeString(draft.description);
    pkt.writeUInt8(draft.type);
    pkt.writeUInt8(draft.repeatOption);
    pkt.writeUInt32(draft.maxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(draft.dungeonId));
    pkt.writeUInt32(core::packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(core::packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(draft.flags);
    sender_.send(pkt);
    LOG_INFO("updateCalendarEvent: ", eventId, " '", draft.title, "'");
}

void CalendarGateway::sendRemove(uint64_t eventId, uint64_t inviteId) {
    // The server reads the id and discards the rest, but the rest is still
    // written: it calls rfinish() rather than stopping, so a short packet is
    // a short read on its side rather than a tidy one.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_REMOVE_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(0);   // flags
    sender_.send(pkt);
    LOG_INFO("removeCalendarEvent: ", eventId);
}

void CalendarGateway::sendInviteStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t status) {
    // packedGuid invitee, eventId, inviteId, ownerInviteId, status
    // (CalendarHandler.cpp:695). Setting someone *else's* status, which is what
    // an event's owner does to confirm or bench them - the player's own answer
    // is CMSG_CALENDAR_EVENT_RSVP and a different packet entirely.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_STATUS));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(0);   // ownerInviteId
    pkt.writeUInt8(status);
    sender_.send(pkt);
    LOG_INFO("setCalendarInviteStatus: invitee ", inviteeGuid, " status ", status);
}

void CalendarGateway::sendModeratorStatus(uint64_t inviteeGuid, uint64_t eventId, uint64_t inviteId, uint8_t rank) {
    // The same shape with a rank in place of the status
    // (CalendarHandler.cpp:727).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_MODERATOR_STATUS));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(0);   // ownerInviteId
    pkt.writeUInt8(rank);
    sender_.send(pkt);
    LOG_INFO("setCalendarInviteModerator: invitee ", inviteeGuid, " rank ", rank);
}

void CalendarGateway::sendEventRequest(uint64_t eventId) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_EVENT));
    pkt.writeUInt64(eventId);
    sender_.send(pkt);
    LOG_INFO("requestCalendarEvent: ", eventId);
}

void CalendarGateway::sendAdd(const calendar::CalendarEventDraft& draft) {
    // title, description, type, repeatable, maxInvites, dungeonId, then two
    // packed times and the flags - CalendarHandler.cpp:228.
    //
    // The second time is unused by the server (it is stored and never read),
    // but it is read off the wire, so leaving it out shifts the flags into it
    // and the flags off the end.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_ADD_EVENT));
    pkt.writeString(draft.title);
    pkt.writeString(draft.description);
    pkt.writeUInt8(draft.type);
    pkt.writeUInt8(draft.repeatOption);
    pkt.writeUInt32(draft.maxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(draft.dungeonId));
    pkt.writeUInt32(core::packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(core::packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(draft.flags);
    sender_.send(pkt);
    LOG_INFO("createCalendarEvent: '", draft.title, "' type ", draft.type,
             " on ", draft.eventTime.day, "/", draft.eventTime.month, "/",
             draft.eventTime.fullYear());
}

void CalendarGateway::sendRsvp(uint64_t eventId, uint64_t inviteId, uint32_t status) {
    // uint64 eventId, uint64 inviteId, uint32 status - CalendarHandler.cpp:630.
    // The status is a CalendarInviteStatus: 1 accepted, 2 declined, 8
    // tentative, 9 removed. The server answers with an event-status alert and
    // clears the pending action, so nothing is assumed here.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_RSVP));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(status);
    sender_.send(pkt);
    LOG_INFO("respondToCalendarInvite: event ", eventId, " invite ", inviteId,
             " status ", status);
}

void CalendarGateway::sendCalendarRequest() {
    // CMSG_CALENDAR_GET_CALENDAR has no payload
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_CALENDAR));
    sender_.send(pkt);
    LOG_INFO("requestCalendar: sent CMSG_CALENDAR_GET_CALENDAR");
}

void CalendarGateway::sendPendingCountRequest() {
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_NUM_PENDING));
    sender_.send(pkt);
}

CommandRefusal calendarCommandRefusal(uint8_t result) {
    switch (result) {
        case 1:  return CommandRefusal::Internal;
        case 2:  return CommandRefusal::GuildEventLimit;
        case 3:  return CommandRefusal::EventLimit;
        case 4:  return CommandRefusal::CannotInvite;
        case 5:  return CommandRefusal::NoInvitesLeft;
        case 6:  return CommandRefusal::InvalidDate;
        case 7:  return CommandRefusal::CannotInviteSelf;
        case 8:  return CommandRefusal::CannotModify;
        case 9:  return CommandRefusal::NotInvited;
        case 10: return CommandRefusal::AlreadyInvited;
        case 11: return CommandRefusal::PlayerNotFound;
        case 12: return CommandRefusal::NotEnoughFocus;
        case 13: return CommandRefusal::EventLocked;
        case 14: return CommandRefusal::EventDeleted;
        case 15: return CommandRefusal::NotModerator;
        default: return CommandRefusal::Other;
    }
}

namespace {

const char* refusalText(CommandRefusal reason) {
    switch (reason) {
        case CommandRefusal::Internal:         return "Calendar: Internal error.";
        case CommandRefusal::GuildEventLimit:  return "Calendar: Guild event limit reached.";
        case CommandRefusal::EventLimit:       return "Calendar: Event limit reached.";
        case CommandRefusal::CannotInvite:     return "Calendar: You cannot invite that player.";
        case CommandRefusal::NoInvitesLeft:    return "Calendar: No invites remaining.";
        case CommandRefusal::InvalidDate:      return "Calendar: Invalid date.";
        case CommandRefusal::CannotInviteSelf: return "Calendar: Cannot invite yourself.";
        case CommandRefusal::CannotModify:     return "Calendar: Cannot modify this event.";
        case CommandRefusal::NotInvited:       return "Calendar: Not invited.";
        case CommandRefusal::AlreadyInvited:   return "Calendar: Already invited.";
        case CommandRefusal::PlayerNotFound:   return "Calendar: Player not found.";
        case CommandRefusal::NotEnoughFocus:   return "Calendar: Not enough focus.";
        case CommandRefusal::EventLocked:      return "Calendar: Event locked.";
        case CommandRefusal::EventDeleted:     return "Calendar: Event deleted.";
        case CommandRefusal::NotModerator:     return "Calendar: Not a moderator.";
        case CommandRefusal::Other:            break;
    }
    return "Calendar: Command failed.";
}

/// "Molten Core (25 Player)", or the map's number when it has no name here.
std::string lockoutLabel(SocialQueries& names, uint32_t mapId, uint32_t difficulty) {
    std::string label = names.mapName(mapId);
    if (label.empty()) label = "map #" + std::to_string(mapId);
    if (const char* diff = instanceDifficultyName(difficulty)) label += std::string(" (") + diff + ")";
    return label;
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeCalendarNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, SocialQueries& names) {
    using namespace calendar;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name);
    };

    // The addon reads the whole calendar on this and nothing else announces
    // that it arrived.
    s.push_back(events.subscribe<CalendarListUpdated>([addon](const auto&) { addon("CALENDAR_UPDATE_EVENT_LIST"); }));
    s.push_back(events.subscribe<EventDetailUpdated>([addon](const auto&) { addon("CALENDAR_UPDATE_EVENT"); }));
    // The minimap's date button carries the indicator: gametime.lua shows
    // GameTimeCalendarInvitesTexture when the count rises and hides it at
    // zero, and it only ever asks on this event and on PLAYER_ENTERING_WORLD.
    s.push_back(events.subscribe<PendingInvitesChanged>([addon](const auto&) {
        addon("CALENDAR_UPDATE_PENDING_INVITES");
    }));
    s.push_back(events.subscribe<PendingInvitesWaiting>([line](const PendingInvitesWaiting& e) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "You have %u pending calendar invite%s.", e.count, e.count == 1 ? "" : "s");
        line(buf);
    }));
    s.push_back(events.subscribe<InviteAlerted>([line](const InviteAlerted& e) {
        line(e.title.empty() ? "You have a new calendar invite." : "Calendar invite: " + e.title);
    }));
    s.push_back(events.subscribe<CommandRefused>([line](const CommandRefused& e) { line(refusalText(e.reason)); }));
    // The chat line is the whole of it, deliberately: an RSVP changing is
    // CALENDAR_UPDATE_INVITE_LIST in retail, and both are read by a calendar
    // that would re-read nothing, since no invite list is kept for either.
    s.push_back(events.subscribe<InviteStatusChanged>([line](const InviteStatusChanged& e) {
        if (e.title.empty()) return;
        // 0 invited, 1 accepted, 2 declined, 3 confirmed, 4 out, 5 standby,
        // 6 signed up, 7 not signed up, 8 tentative.
        static constexpr const char* kRsvpStatus[] = {
            "invited", "accepted", "declined", "confirmed",
            "out", "on standby", "signed up", "not signed up", "tentative"};
        const char* status = e.status < 9 ? kRsvpStatus[e.status] : "unknown";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Calendar event '%s': your RSVP is %s.", e.title.c_str(), status);
        line(buf);
    }));
    s.push_back(events.subscribe<LockoutAdded>([line, &names](const LockoutAdded& e) {
        line("Calendar: Raid lockout added for " + lockoutLabel(names, e.mapId, e.difficulty) + ".");
    }));
    s.push_back(events.subscribe<LockoutRemoved>([line, &names](const LockoutRemoved& e) {
        line("Calendar: Raid lockout removed for " + lockoutLabel(names, e.mapId, e.difficulty) + ".");
    }));
    return s;
}

} // namespace wowee::game

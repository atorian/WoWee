#include "game/arena_gateway.hpp"

#include "arena/domain/events.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "network/packet.hpp"

#include <algorithm>

namespace wowee::game {

using arena::ArenaCommand;
using arena::ArenaCommandError;
using arena::ArenaMatchError;
using arena::ArenaTeamChange;

bool ArenaGateway::online() const {
    return sender_.characterInWorld();
}

bool ArenaGateway::connected() const {
    return sender_.connected();
}

void ArenaGateway::sendAccept() {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_ACCEPT));
    sender_.send(packet);
}

void ArenaGateway::sendDecline() {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_DECLINE));
    sender_.send(packet);
}

// Every command is the team id and, for three of them, a player name - the
// shape AzerothCore reads in ArenaTeamHandler.cpp:
// `recvData >> arenaTeamId >> name`.
void ArenaGateway::sendInvite(uint32_t teamId, const std::string& name) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_INVITE));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    sender_.send(packet);
}

void ArenaGateway::sendLeave(uint32_t teamId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_LEAVE));
    packet.writeUInt32(teamId);
    sender_.send(packet);
}

void ArenaGateway::sendRemove(uint32_t teamId, const std::string& name) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_REMOVE));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    sender_.send(packet);
}

void ArenaGateway::sendLeader(uint32_t teamId, const std::string& name) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_LEADER));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    sender_.send(packet);
}

void ArenaGateway::sendDisband(uint32_t teamId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_DISBAND));
    packet.writeUInt32(teamId);
    sender_.send(packet);
}

void ArenaGateway::sendRosterRequest(uint32_t teamId) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_ARENA_TEAM_ROSTER));
    pkt.writeUInt32(teamId);
    sender_.send(pkt);
}

std::optional<arena::ArenaTeamRoster> readArenaTeamRoster(network::Packet& packet) {
    // teamId(4) + unk308(1) + memberCount(4) + teamType(4), then the members.
    //
    // The team type was never read, so the first member's guid was taken from
    // four bytes earlier - the type plus the guid's low half - and every
    // member after it followed from there. The roster was wrong from its first
    // row, and nothing about it looked like a length fault: the packet is long
    // enough either way.
    if (!packet.hasRemaining(13)) return std::nullopt;
    arena::ArenaTeamRoster roster;
    roster.teamId = packet.readUInt32();
    // 3.0.8 added this, and it does more than mark a version: the server writes
    // the two trailing floats per member only when it is set.
    const bool hasTrailingFloats = packet.readUInt8() != 0;
    const uint32_t memberCount = std::min(packet.readUInt32(), 100u);
    /*uint32_t teamType =*/ packet.readUInt32();
    roster.members.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        arena::ArenaTeamMember m;
        m.guid = packet.readUInt64(); m.online = (packet.readUInt8() != 0); m.name = packet.readString();
        // Captain flag, level and class sit between the name and the record -
        // six bytes this went straight past, so the games played were the
        // captain flag, the wins were the level and class with two bytes of the
        // real figure behind them, and so on down the row.
        if (!packet.hasRemaining(6)) break;
        m.isCaptain = (packet.readUInt32() == 0);   // 0 captain, 1 member
        m.level     = packet.readUInt8();
        m.classId   = packet.readUInt8();
        if (!packet.hasRemaining(20)) break;
        m.weekGames = packet.readUInt32(); m.weekWins = packet.readUInt32();
        m.seasonGames = packet.readUInt32(); m.seasonWins = packet.readUInt32(); m.personalRating = packet.readUInt32();
        // Only when the header said so. Taking them whenever eight bytes were
        // left ate the next member's guid on every roster of more than one.
        if (hasTrailingFloats) {
            if (!packet.hasRemaining(8)) break;
            packet.readFloat(); packet.readFloat();
        }
        roster.members.push_back(std::move(m));
    }
    return roster;
}

// ArenaTeamCommandTypes, and it is not a dense range: 2, and everything
// between 4 and 13, are values the server never sends. An old table read it as
// 0..5 in a row, so a quit reported itself as a remove and a change of captain
// fell off the end.
ArenaCommand arenaCommand(uint32_t command) {
    switch (command) {
        case 0x00: return ArenaCommand::Create;
        case 0x01: return ArenaCommand::Invite;
        case 0x03: return ArenaCommand::Quit;
        case 0x0E: return ArenaCommand::Captain;
        default:   return ArenaCommand::Other;
    }
}

// ArenaTeamCommandErrors. The range is sparse - the server sends 0x08 for two
// different refusals and skips most of what lies between.
ArenaCommandError arenaCommandError(uint32_t error) {
    switch (error) {
        case 0x01: return ArenaCommandError::Internal;
        case 0x02: return ArenaCommandError::AlreadyInTeamOfSize;
        case 0x03: return ArenaCommandError::TargetAlreadyInTeamOfSize;
        case 0x04: return ArenaCommandError::AlreadyInvited;
        case 0x05: return ArenaCommandError::TargetAlreadyInvited;
        case 0x06: return ArenaCommandError::NameInvalid;
        case 0x07: return ArenaCommandError::NameTaken;
        case 0x08: return ArenaCommandError::CaptainCannotLeaveOrNoPermission;
        case 0x09: return ArenaCommandError::NotInTeamOfSize;
        case 0x0A: return ArenaCommandError::TargetNotInTeam;
        case 0x0B: return ArenaCommandError::PlayerNotFound;
        case 0x0C: return ArenaCommandError::TargetOpposingFaction;
        case 0x13: return ArenaCommandError::TargetIgnoringYou;
        case 0x15: return ArenaCommandError::TargetLevelTooLow;
        case 0x16: return ArenaCommandError::TargetLevelTooHigh;
        case 0x17: return ArenaCommandError::TeamFull;
        case 0x1B: return ArenaCommandError::TeamNotFound;
        case 0x1E: return ArenaCommandError::LockedForSeason;
        default:   return ArenaCommandError::Other;
    }
}

ArenaTeamChange arenaTeamChange(uint32_t event) {
    switch (event) {
        case 0: return ArenaTeamChange::Joined;
        case 1: return ArenaTeamChange::Left;
        case 2: return ArenaTeamChange::Removed;
        case 3: return ArenaTeamChange::CaptainChanged;
        case 4: return ArenaTeamChange::Disbanded;
        case 5: return ArenaTeamChange::Created;
        default: return ArenaTeamChange::Other;
    }
}

ArenaMatchError arenaMatchError(uint32_t error) {
    switch (error) {
        case 1: return ArenaMatchError::OtherTeamTooSmall;
        case 2: return ArenaMatchError::TeamFull;
        case 3: return ArenaMatchError::NotEnoughMembers;
        case 4: return ArenaMatchError::TooManyMembers;
        default: return ArenaMatchError::Other;
    }
}

namespace {

const char* commandName(ArenaCommand command) {
    switch (command) {
        case ArenaCommand::Create:  return "arena team create";
        case ArenaCommand::Invite:  return "arena team invite";
        case ArenaCommand::Quit:    return "arena team quit";
        case ArenaCommand::Captain: return "arena team captain";
        case ArenaCommand::Other:   break;
    }
    return "arena team";
}

const char* commandErrorText(ArenaCommandError error) {
    switch (error) {
        case ArenaCommandError::Internal:                  return "Internal arena team error.";
        case ArenaCommandError::AlreadyInTeamOfSize:       return "You are already in an arena team of that size.";
        case ArenaCommandError::TargetAlreadyInTeamOfSize: return "That player is already in an arena team of that size.";
        case ArenaCommandError::AlreadyInvited:            return "You have already been invited to an arena team.";
        case ArenaCommandError::TargetAlreadyInvited:      return "That player has already been invited to an arena team.";
        case ArenaCommandError::NameInvalid:               return "That arena team name is invalid.";
        case ArenaCommandError::NameTaken:                 return "That arena team name already exists.";
        case ArenaCommandError::CaptainCannotLeaveOrNoPermission:
            return "The captain cannot leave; permissions denied.";
        case ArenaCommandError::NotInTeamOfSize:           return "You are not in an arena team of that size.";
        case ArenaCommandError::TargetNotInTeam:           return "That player is not in your arena team.";
        case ArenaCommandError::PlayerNotFound:            return "No player of that name was found.";
        case ArenaCommandError::TargetOpposingFaction:     return "That player is of the opposing faction.";
        case ArenaCommandError::TargetIgnoringYou:         return "That player is ignoring you.";
        case ArenaCommandError::TargetLevelTooLow:         return "That player's level is too low.";
        case ArenaCommandError::TargetLevelTooHigh:        return "That player's level is too high.";
        case ArenaCommandError::TeamFull:                  return "The arena team is full.";
        case ArenaCommandError::TeamNotFound:              return "No arena team of that name was found.";
        case ArenaCommandError::LockedForSeason:           return "Arena teams are locked for the season.";
        case ArenaCommandError::Other:                     break;
    }
    return "The arena team command was refused.";
}

/// Whichever name the result is about. A create names the team, an invite the
/// player, and both are sent for the ones that concern both.
std::string about(const std::string& teamName, const std::string& playerName) {
    std::string out = teamName;
    if (!playerName.empty()) out = out.empty() ? playerName : out + " / " + playerName;
    return out;
}

std::string teamChangeText(const arena::ArenaTeamChanged& e) {
    const std::string& a = e.first;
    const std::string& b = e.second;
    switch (e.change) {
        case ArenaTeamChange::Joined:
            return a.empty() ? "A player has joined your arena team." : a + " has joined your arena team.";
        case ArenaTeamChange::Left:
            return a.empty() ? "A player has left the arena team." : a + " has left the arena team.";
        case ArenaTeamChange::Removed:
            return (!a.empty() && !b.empty()) ? a + " has been removed from the arena team by " + b + "."
                                              : "A player has been removed from the arena team.";
        case ArenaTeamChange::CaptainChanged:
            return a.empty() ? "The arena team captain has changed." : a + " is now the arena team captain.";
        case ArenaTeamChange::Disbanded:
            return "Your arena team has been disbanded.";
        case ArenaTeamChange::Created:
            return a.empty() ? "Your arena team has been created." : "Arena team \"" + a + "\" has been created.";
        case ArenaTeamChange::Other:
            break;
    }
    std::string msg = "Arena team event " + std::to_string(e.code);
    if (!a.empty()) msg += ": " + a;
    return msg;
}

std::string matchErrorText(const arena::ArenaMatchRefused& e) {
    switch (e.error) {
        case ArenaMatchError::OtherTeamTooSmall: return "The other team is not big enough.";
        case ArenaMatchError::TeamFull:          return "That team is full.";
        case ArenaMatchError::NotEnoughMembers:  return "Not enough members to start.";
        case ArenaMatchError::TooManyMembers:    return "Too many members.";
        case ArenaMatchError::Other:             break;
    }
    return "Arena error (code " + std::to_string(e.code) + ")";
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeArenaNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace arena;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name);
    };

    // A team's name arriving is what the PvP panel redraws on. Without it the
    // arena tabs kept whatever they were built with, so a team queried after
    // the panel opened stayed nameless until it was closed and opened again.
    s.push_back(events.subscribe<TeamsUpdated>([addon](const auto&) { addon("ARENA_TEAM_UPDATE"); }));
    // The team detail window redraws on this and on nothing else. Fired with
    // no arguments, and that is the whole contract: pvpframe reads arg1 as
    // "the roster you hold is stale, ask for it again", so any argument would
    // answer this roster by requesting another one, forever. A zero would do
    // it too, zero being true in Lua.
    s.push_back(events.subscribe<RosterUpdated>([addon](const auto&) { addon("ARENA_TEAM_ROSTER_UPDATE"); }));

    s.push_back(events.subscribe<ArenaCommandSucceeded>([line](const ArenaCommandSucceeded& e) {
        const std::string what = about(e.teamName, e.playerName);
        line(std::string(commandName(e.command)) + " succeeded" + (what.empty() ? "." : ": " + what));
    }));
    s.push_back(events.subscribe<ArenaCommandRefused>([line](const ArenaCommandRefused& e) {
        const std::string what = about(e.teamName, e.playerName);
        line(std::string(commandName(e.command)) + " failed" + (what.empty() ? "" : " (" + what + ")") + ": " +
             commandErrorText(e.error));
    }));
    s.push_back(events.subscribe<InvitedToArenaTeam>([line](const InvitedToArenaTeam& e) {
        line(e.inviter + " has invited you to join " + e.teamName + ".");
    }));
    s.push_back(events.subscribe<ArenaTeamChanged>([line](const ArenaTeamChanged& e) { line(teamChangeText(e)); }));
    s.push_back(events.subscribe<ArenaMatchRefused>([line](const ArenaMatchRefused& e) { line(matchErrorText(e)); }));
    return s;
}

} // namespace wowee::game

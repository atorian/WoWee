#include "game/battleground_gateway.hpp"

#include "battleground/domain/events.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/entity.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_utils.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/ui_errors.hpp"
#include "network/packet.hpp"

#include <algorithm>

namespace wowee::game {

using battleground::JoinRefusal;

bool BattlegroundGateway::online() const {
    return sender_.characterInWorld();
}

void BattlegroundGateway::sendPort(uint8_t arenaType, uint32_t bgTypeId, bool accept) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_PORT));
    pkt.writeUInt8(arenaType); pkt.writeUInt8(0x00); pkt.writeUInt32(bgTypeId);
    pkt.writeUInt16(0x0000); pkt.writeUInt8(accept ? 1 : 0);
    sender_.send(pkt);
}

// CMSG_BATTLEMASTER_JOIN: which battlemaster is being spoken to, which
// battleground, which instance of it, and whether the whole group is coming.
// An instance of zero means "first available", which is what the top row of
// the battlefield list is.
void BattlegroundGateway::sendJoin(uint64_t battlemaster, uint32_t bgTypeId, uint32_t instanceId, bool asGroup) {
    network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEMASTER_JOIN));
    packet.writeUInt64(battlemaster);
    packet.writeUInt32(bgTypeId);
    packet.writeUInt32(instanceId);
    packet.writeUInt8(asGroup ? 1 : 0);
    sender_.send(packet);
}

// CMSG_BATTLEFIELD_LIST: ask which instances of one battleground are running.
void BattlegroundGateway::sendListRequest(uint32_t bgTypeId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEFIELD_LIST));
    packet.writeUInt32(bgTypeId);
    // WotLK added two bytes: where the request came from, and whether the
    // player wants experience from it. Classic and TBC read the type id alone,
    // and trailing bytes there would be read as the next packet.
    if (!isPreWotlk()) {
        packet.writeUInt8(0);  // fromWhere: 0 = the battlemaster list frame
        packet.writeUInt8(1);  // canGainXP
    }
    sender_.send(packet);
}

// CMSG_REPORT_PVP_AFK: this player is not participating.
void BattlegroundGateway::sendReportAfk(uint64_t playerGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_REPORT_PVP_AFK));
    pkt.writeUInt64(playerGuid);
    sender_.send(pkt);
}

// Leave the battleground the player is standing in, which is a different thing
// from declining an invitation to one - that is CMSG_BATTLEFIELD_PORT with a
// refusal, this is the player walking out of a match already joined.
void BattlegroundGateway::sendLeave() {
    network::Packet pkt(wireOpcode(Opcode::CMSG_LEAVE_BATTLEFIELD));
    sender_.send(pkt);
}

void BattlegroundGateway::sendScoreRequest() {
    network::Packet pkt(wireOpcode(Opcode::MSG_PVP_LOG_DATA));
    sender_.send(pkt);
}

// MSG_BATTLEGROUND_PLAYER_POSITIONS is a request the server answers with the
// same opcode.
void BattlegroundGateway::sendPositionsRequest() {
    network::Packet pkt(wireOpcode(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS));
    sender_.send(pkt);
}

std::string BattlegroundGateway::playerName(uint64_t guid) const {
    return world_.playerName(guid);
}

// AzerothCore drops a position request outright when the player has no
// battleground, so asking anywhere else is bytes for nothing.
bool BattlegroundGateway::inBattleground() const {
    return world_.battlegroundMap(world_.currentMapId());
}

std::optional<battleground::AvailableBgInfo> readBattlefieldList(network::Packet& packet) {
    if (!packet.hasRemaining(5)) return std::nullopt;
    battleground::AvailableBgInfo info;
    info.bgTypeId = packet.readUInt32();
    info.isRegistered = packet.readUInt8() != 0;
    const bool isWotlk = isActiveExpansion("wotlk");
    const bool isTbc = isActiveExpansion("tbc");
    if (isTbc || isWotlk) {
        if (!packet.hasRemaining(1)) return std::nullopt;
        info.isHoliday = packet.readUInt8() != 0;
    }
    if (isWotlk) {
        if (!packet.hasRemaining(8)) return std::nullopt;
        info.minLevel = packet.readUInt32();
        info.maxLevel = packet.readUInt32();
    }
    if (!packet.hasRemaining(4)) return std::nullopt;
    const uint32_t count = std::min(packet.readUInt32(), 256u);
    info.instanceIds.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(4)) break;
        info.instanceIds.push_back(packet.readUInt32());
    }
    return info;
}

std::vector<battleground::BgPlayerPosition> readBattlegroundPositions(network::Packet& packet) {
    std::vector<battleground::BgPlayerPosition> positions;
    for (int grp = 0; grp < 2; ++grp) {
        if (!packet.hasRemaining(4)) break;
        const uint32_t count = packet.readUInt32();
        for (uint32_t i = 0; i < count && packet.getRemainingSize() >= 16; ++i) {
            battleground::BgPlayerPosition pos;
            pos.guid = packet.readUInt64();
            pos.wowX = packet.readFloat();
            pos.wowY = packet.readFloat();
            pos.group = grp;
            positions.push_back(pos);
        }
    }
    return positions;
}

std::optional<battleground::BgScoreboardData> readPvpLog(network::Packet& packet, const EntityManager& entities) {
    auto remaining = [&]() { return packet.getRemainingSize(); };
    if (remaining() < 1) return std::nullopt;
    // Whether there is a team byte after the guid is answered by the source
    // rather than by a byte count, and the answer is "in an arena, yes; in a
    // battleground, no". They are two different rows written by two different
    // overrides, and the type byte at the top of this packet says which one
    // follows.
    battleground::BgScoreboardData board;
    board.isArena = (packet.readUInt8() != 0);
    if (board.isArena) {
        for (auto& arenaTeam : board.arenaTeams) {
            if (remaining() < 20) { packet.skipAll(); return std::nullopt; }
            arenaTeam.ratingChange = packet.readUInt32();
            arenaTeam.newRating = packet.readUInt32();
            packet.readUInt32(); packet.readUInt32(); packet.readUInt32();
            arenaTeam.teamName = remaining() > 0 ? packet.readString() : "";
        }
    }
    // Ended, and the winner, BEFORE the player count. This was read after the
    // players, where there is nothing left to read it from.
    if (remaining() < 1) return std::nullopt;
    board.hasWinner = (packet.readUInt8() != 0);
    if (board.hasWinner) {
        if (remaining() < 1) return std::nullopt;
        board.winner = packet.readUInt8();
    }

    if (remaining() < 4) return std::nullopt;
    const uint32_t playerCount = packet.readUInt32();
    board.players.reserve(playerCount);
    for (uint32_t i = 0; i < playerCount && remaining() >= 12; ++i) {
        battleground::BgPlayerScore ps;
        ps.guid = packet.readUInt64();
        // Two different rows, and the type byte at the top says which.
        //
        // BattlegroundScore::AppendToPacket - killing blows, honourable kills,
        // deaths, bonus honour, damage, healing. No team.
        // ArenaScore::AppendToPacket - killing blows, a team BYTE, damage,
        // healing. No honourable kills, deaths or honour at all.
        //
        // This read one row that was neither: a team byte from the arena
        // shape followed by the battleground's four counters, then a stat block
        // with null-terminated names that no core writes. Everything after the
        // guid was off by a byte, and damage and healing were skipped entirely
        // - which is why GetBattlefieldScore answered zero for both.
        ps.killingBlows = packet.readUInt32();
        if (board.isArena) {
            if (remaining() < 1) { board.players.push_back(std::move(ps)); break; }
            ps.team = packet.readUInt8();
            ps.hasTeam = true;
        } else {
            if (remaining() < 12) { board.players.push_back(std::move(ps)); break; }
            ps.honorableKills = packet.readUInt32();
            ps.deaths         = packet.readUInt32();
            ps.bonusHonor     = packet.readUInt32();
            // No team on the wire for a battleground. The byte that used to
            // fill this was the low byte of the killing blows, so the faction
            // tabs were already filtering on nothing; leaving it unset is the
            // same amount of information, honestly labelled.
        }
        if (remaining() < 8) { board.players.push_back(std::move(ps)); break; }
        ps.damageDone  = packet.readUInt32();
        ps.healingDone = packet.readUInt32();

        if (auto ent = entities.getEntity(ps.guid);
            ent && (ent->getType() == ObjectType::PLAYER || ent->getType() == ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<Unit>(ent);
            if (!unit->getName().empty()) ps.name = unit->getName();
        }

        // BuildObjectivesBlock: a count and that many bare values. The names
        // this used to read are not on the wire - BattlegroundWS writes
        // uint32(2) and two numbers - so every value past the first came from
        // the wrong offset.
        if (remaining() >= 4) {
            const uint32_t statCount = packet.readUInt32();
            for (uint32_t st = 0; st < statCount && remaining() >= 4; ++st)
                ps.bgStats.emplace_back(std::string(), packet.readUInt32());
        }
        board.players.push_back(std::move(ps));
    }
    return board;
}

JoinRefusal joinRefusal(int32_t result) {
    switch (result) {
        case 0:   return JoinRefusal::NotEligible;
        case -2:  return JoinRefusal::Deserter;
        case -3:  return JoinRefusal::ArenaPartySize;
        case -4:  return JoinRefusal::TooManyQueues;
        case -5:  return JoinRefusal::RatedWhileQueued;
        case -6:  return JoinRefusal::QueuedWhileRated;
        case -7:  return JoinRefusal::TeamLeftArenaQueue;
        case -8:  return JoinRefusal::InBattleground;
        case -10: return JoinRefusal::LevelRange;
        case -11: return JoinRefusal::MemberUnavailable;
        case -12: return JoinRefusal::GroupJoinFailed;
        case -13: return JoinRefusal::InDungeonSystem;
        case -14: return JoinRefusal::InRandomQueue;
        case -15: return JoinRefusal::RandomWhileQueued;
        default:  return JoinRefusal::Other;
    }
}

namespace {

std::string joinRefusedText(const battleground::JoinRefused& e) {
    switch (e.reason) {
        case JoinRefusal::NotEligible:        return "Your group has joined a battleground queue, but you are not eligible";
        case JoinRefusal::Deserter:           return "You cannot join the battleground yet because you or one of your party members is flagged as a Deserter.";
        case JoinRefusal::ArenaPartySize:     return "Incorrect party size for this arena.";
        case JoinRefusal::TooManyQueues:      return "You can only be queued for 2 battles at once";
        case JoinRefusal::RatedWhileQueued:   return "You cannot queue for a rated match while queued for other battles";
        case JoinRefusal::QueuedWhileRated:   return "You cannot queue for another battle while queued for a rated arena match";
        case JoinRefusal::TeamLeftArenaQueue: return "Your team has left the arena queue";
        case JoinRefusal::InBattleground:     return "You can't do that in a battleground.";
        case JoinRefusal::LevelRange:         return "Cannot join the queue unless all members of your party are in the same battleground level range.";
        case JoinRefusal::MemberUnavailable:  return e.member + " was unavailable to join the queue.";
        case JoinRefusal::GroupJoinFailed:    return "Join as a group failed";
        case JoinRefusal::InDungeonSystem:    return "You cannot queue for a battleground or arena while using the dungeon system.";
        case JoinRefusal::InRandomQueue:      return "Can't do that while in a Random Battleground queue.";
        case JoinRefusal::RandomWhileQueued:  return "Can't queue for Random Battleground while in another Battleground queue.";
        case JoinRefusal::Other:              break;
    }
    return "Could not join the battleground queue (" + std::to_string(e.code) + ").";
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeBattlegroundNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace battleground;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    // A refusal goes on the error line and in the chat, where it stays.
    auto failure = [&events, &frameXml, &chat](const std::string& text) {
        raiseUiError(events, frameXml, text);
        chat.addSystemChatMessage(text);
    };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    s.push_back(events.subscribe<QueueStatusChanged>([line](const QueueStatusChanged& e) {
        LOG_WARNING("Battleground queue ", e.slot + 1, ": status ", e.statusId,
                    " for type ", e.bgTypeId, " (", e.bgName, "), average wait ",
                    e.avgWaitSec, " s, waited ", e.waitedSec, " s");
        switch (e.statusId) {
            case 1: line("Queued for " + e.bgName + "."); break;
            case 2: line(e.bgName + " is ready!"); break;
            case 3: line("Entered " + e.bgName + "."); break;
            default: break;
        }
    }));
    s.push_back(events.subscribe<QueueSlotUpdated>([addon](const QueueSlotUpdated& e) {
        addon("UPDATE_BATTLEFIELD_STATUS", {std::to_string(e.slot)});
    }));
    s.push_back(events.subscribe<QueueDeclined>([line](const QueueDeclined& e) {
        line(e.wasInvite ? "Battleground invitation declined." : "Left the queue for " + e.bgName + ".");
    }));
    s.push_back(events.subscribe<NoInvitation>([line](const auto&) { line("No battleground invitation pending."); }));
    s.push_back(events.subscribe<EnteringBattleground>([line](const auto&) {
        line("Accepting battleground invitation...");
    }));
    // The scoreboard is drawn from this and asked for it by name -
    // RequestBattlefieldScoreData sends the query, UPDATE_BATTLEFIELD_SCORE is
    // the answer arriving.
    s.push_back(events.subscribe<ScoreUpdated>([addon](const auto&) { addon("UPDATE_BATTLEFIELD_SCORE"); }));
    // A realm that keeps a battleground back - ChromieCraft opens the random
    // one at level sixty, and only through a command of its own - drops the
    // join unanswered, and the panel then did nothing at all when its button
    // was pressed.
    s.push_back(events.subscribe<JoinUnanswered>([failure](const JoinUnanswered& e) {
        std::string name = battlegroundName(e.bgTypeId);
        if (name.empty()) name = "that battleground";
        LOG_WARNING("Battleground join unanswered: type ", e.bgTypeId);
        failure("The server did not answer the request to queue for " + name +
                " - this realm may not offer it to you.");
    }));
    s.push_back(events.subscribe<GroupQueued>([line](const GroupQueued& e) {
        std::string name = battlegroundName(e.bgTypeId);
        if (name.empty()) name = "a battleground";
        line("Your group has joined the queue for " + name);
    }));
    // A Deserter or a third queue used to be reported as success, and nothing
    // appeared on the minimap.
    s.push_back(events.subscribe<JoinRefused>([failure](const JoinRefused& e) {
        const std::string msg = joinRefusedText(e);
        LOG_WARNING("Battleground join refused: ", e.code, " - ", msg);
        failure(msg);
    }));
    s.push_back(events.subscribe<PortDenied>([failure](const auto&) { failure("Battlefield port denied."); }));
    s.push_back(events.subscribe<RemovedFromQueue>([line](const auto&) {
        line("You have been removed from the PvP queue.");
    }));
    s.push_back(events.subscribe<JoinedQueue>([line](const auto&) { line("You have joined the battleground queue."); }));
    s.push_back(events.subscribe<PlayerEntered>([line](const PlayerEntered& e) {
        line(e.name + " has entered the battleground.");
    }));
    s.push_back(events.subscribe<PlayerLeft>([line](const PlayerLeft& e) { line(e.name + " has left the battleground."); }));
    s.push_back(events.subscribe<AfkReportAnswered>([line, &events, &frameXml](const AfkReportAnswered& e) {
        if (e.accepted) line("AFK report submitted.");
        else raiseUiError(events, frameXml, "Cannot report that player as AFK right now.");
    }));
    return s;
}

} // namespace wowee::game

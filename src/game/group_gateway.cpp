#include "game/group_gateway.hpp"

#include "foundation/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_utils.hpp"
#include "game/group_defines.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/spell_defines.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "group/domain/events.hpp"
#include "network/packet.hpp"

#include <cstdio>

namespace wowee::game {

bool GroupGateway::canSend() const {
    return sender_.characterInWorld();
}

uint64_t GroupGateway::playerGuid() const {
    return world_.characterGuid();
}

void GroupGateway::sendInvite(const std::string& name) {
    LOG_INFO("Sending CMSG_GROUP_INVITE to '", name, "'");
    sender_.send(GroupInvitePacket::build(name));
}

void GroupGateway::sendAccept() {
    LOG_INFO("Sending CMSG_GROUP_ACCEPT");
    sender_.send(GroupAcceptPacket::build());
}

void GroupGateway::sendDecline() {
    sender_.send(GroupDeclinePacket::build());
}

void GroupGateway::sendUninvite(const std::string& name) {
    sender_.send(GroupUninvitePacket::build(name));
}

void GroupGateway::sendLeave() {
    sender_.send(GroupDisbandPacket::build());
}

void GroupGateway::sendConvertToRaid() {
    sender_.send(GroupRaidConvertPacket::build());
    LOG_INFO("Sent CMSG_GROUP_RAID_CONVERT");
}

// The server checks that this player leads the group and that the target is in
// it, so there is nothing to validate here beyond having a guid to send.
void GroupGateway::sendLeader(uint64_t guid) {
    sender_.send(GroupSetLeaderPacket::build(guid));
}

// CMSG_LOOT_METHOD carries all three settings together, so changing one means
// resending the other two as they stand.
void GroupGateway::sendLootMethod(uint32_t method, uint64_t masterGuid, uint32_t threshold) {
    sender_.send(SetLootMethodPacket::build(method, threshold, masterGuid));
    LOG_INFO("Set loot method ", method, " threshold ", threshold);
}

// MSG_PARTY_ASSIGNMENT: which assignment (0 main tank, 1 main assist), whether
// it is being given or taken away, and who.
void GroupGateway::sendAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    network::Packet packet(wireOpcode(Opcode::MSG_PARTY_ASSIGNMENT));
    packet.writeUInt8(assignment);
    packet.writeUInt8(apply ? 1 : 0);
    packet.writeUInt64(guid);
    sender_.send(packet);
}

// A guid and a flag; AzerothCore drops it unless the sender leads the group -
// which is the same test the unit menu makes before offering the entry.
void GroupGateway::sendAssistant(uint64_t guid, bool apply) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_ASSISTANT_LEADER));
    packet.writeUInt64(guid);
    packet.writeUInt8(apply ? 1 : 0);
    sender_.send(packet);
}

// The server takes the player's name and the destination group as a zero-based
// index.
void GroupGateway::sendSubgroup(const std::string& name, uint8_t group) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP));
    packet.writeString(name);
    packet.writeUInt8(group);
    sender_.send(packet);
}

// Exchange two raid members' places. Used when the destination group is already
// full, which is why it is a separate packet rather than two moves.
void GroupGateway::sendSwapSubgroup(const std::string& first, const std::string& second) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_SWAP_SUB_GROUP));
    packet.writeString(first);
    packet.writeString(second);
    sender_.send(packet);
}

void GroupGateway::sendRaidMark(uint8_t icon, uint64_t guid) {
    sender_.send(RaidTargetUpdatePacket::build(icon, guid));
}

void GroupGateway::sendReadyCheck() {
    sender_.send(ReadyCheckPacket::build());
}

void GroupGateway::sendReadyCheckAnswer(bool ready) {
    sender_.send(ReadyCheckConfirmPacket::build(ready));
}

std::optional<group::MemberStats> readPartyMemberStats(network::Packet& packet, bool isFull,
                                                       SocialQueries& dispelTypes,
                                                       std::optional<std::vector<AuraSlot>>& auras) {
    namespace F = group::StatsField;
    auto remaining = [&]() { return packet.getRemainingSize(); };
    const bool isWotLK = isActiveExpansion("wotlk");

    if (isFull) { if (remaining() < 1) return std::nullopt; packet.readUInt8(); }

    const bool tbc = isActiveExpansion("tbc");
    if (remaining() < (tbc ? 8u : 1u)) return std::nullopt;
    group::MemberStats s;
    s.guid = tbc ? packet.readUInt64() : packet.readPackedGuid();
    if (remaining() < 4) return std::nullopt;
    s.updateFlags = packet.readUInt32();

    const uint32_t f = s.updateFlags;
    if (f & F::Status)    { if (remaining() >= 2) { s.onlineStatus = packet.readUInt16(); s.present |= F::Status; } }
    if (f & F::CurHealth) {
        if (isWotLK) { if (remaining() >= 4) { s.curHealth = packet.readUInt32(); s.present |= F::CurHealth; } }
        else         { if (remaining() >= 2) { s.curHealth = packet.readUInt16(); s.present |= F::CurHealth; } }
    }
    if (f & F::MaxHealth) {
        if (isWotLK) { if (remaining() >= 4) { s.maxHealth = packet.readUInt32(); s.present |= F::MaxHealth; } }
        else         { if (remaining() >= 2) { s.maxHealth = packet.readUInt16(); s.present |= F::MaxHealth; } }
    }
    if (f & F::PowerType) { if (remaining() >= 1) { s.powerType = packet.readUInt8(); s.present |= F::PowerType; } }
    if (f & F::CurPower)  { if (remaining() >= 2) { s.curPower = packet.readUInt16(); s.present |= F::CurPower; } }
    if (f & F::MaxPower)  { if (remaining() >= 2) { s.maxPower = packet.readUInt16(); s.present |= F::MaxPower; } }
    if (f & F::Level)     { if (remaining() >= 2) { s.level = packet.readUInt16(); s.present |= F::Level; } }
    if (f & F::Zone)      { if (remaining() >= 2) { s.zoneId = packet.readUInt16(); s.present |= F::Zone; } }
    if (f & F::Position) {
        if (remaining() >= 4) {
            s.posX = static_cast<int16_t>(packet.readUInt16());
            s.posY = static_cast<int16_t>(packet.readUInt16());
            s.present |= F::Position;
        }
    }
    if (f & F::Auras) {
        if (remaining() >= 8) {
            const uint64_t auraMask = packet.readUInt64();
            std::vector<AuraSlot> read;
            for (int i = 0; i < 64; ++i) {
                if (!(auraMask & (uint64_t(1) << i))) continue;
                AuraSlot a;
                a.level = static_cast<uint8_t>(i);
                if (isWotLK) {
                    if (remaining() < 5) break;
                    a.spellId = packet.readUInt32();
                    a.flags   = packet.readUInt8();
                } else {
                    if (remaining() < 2) break;
                    a.spellId = packet.readUInt16();
                    if (dispelTypes.spellDispelType(a.spellId) > 0) a.flags = 0x80;
                }
                if (a.spellId != 0) read.push_back(a);
            }
            auras = std::move(read);
        }
    }
    // Pet fields and the vehicle seat: read past, not kept.
    if (f & 0x0400) { if (remaining() >= 8) packet.readUInt64(); }
    if (f & 0x0800) { if (remaining() > 0) packet.readString(); }
    if (f & 0x1000) { if (remaining() >= 2) packet.readUInt16(); }
    if (f & 0x2000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (f & 0x4000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (f & 0x8000) { if (remaining() >= 1) packet.readUInt8(); }
    if (f & 0x10000) { if (remaining() >= 2) packet.readUInt16(); }
    if (f & 0x20000) { if (remaining() >= 2) packet.readUInt16(); }
    if (f & 0x40000) {
        if (remaining() >= 8) {
            const uint64_t petAuraMask = packet.readUInt64();
            for (int i = 0; i < 64; ++i) {
                if (!(petAuraMask & (uint64_t(1) << i))) continue;
                if (isWotLK) { if (remaining() < 5) break; packet.readUInt32(); packet.readUInt8(); }
                else { if (remaining() < 2) break; packet.readUInt16(); }
            }
        }
    }
    if (isWotLK && (f & 0x80000)) { if (remaining() >= 4) packet.readUInt32(); }
    return s;
}

namespace {

std::string partyError(const group::PartyCommandFailed& e) {
    using group::PartyResult;
    const char* text = "Party command failed.";
    switch (e.result) {
        case PartyResult::BAD_PLAYER_NAME:        text = "No player named \"%s\" is currently online."; break;
        case PartyResult::TARGET_NOT_IN_GROUP:    text = "%s is not in your group."; break;
        case PartyResult::TARGET_NOT_IN_INSTANCE: text = "%s is not in your instance."; break;
        case PartyResult::GROUP_FULL:             text = "Your party is full."; break;
        case PartyResult::ALREADY_IN_GROUP:       text = "%s is already in a group."; break;
        case PartyResult::NOT_IN_GROUP:           text = "You are not in a group."; break;
        case PartyResult::NOT_LEADER:             text = "You are not the group leader."; break;
        case PartyResult::PLAYER_WRONG_FACTION:   text = "%s is the wrong faction for this group."; break;
        case PartyResult::IGNORING_YOU:           text = "%s is ignoring you."; break;
        case PartyResult::LFG_PENDING:            text = "You cannot do that while in a LFG queue."; break;
        case PartyResult::INVITE_RESTRICTED:      text = "Target is not accepting group invites."; break;
        default: break;
    }
    std::string msg = text;
    if (!e.name.empty()) {
        if (auto pos = msg.find("%s"); pos != std::string::npos) msg.replace(pos, 2, e.name);
    }
    return msg;
}

const char* assignmentName(group::Assignment a) {
    return a == group::Assignment::MainTank ? "Main tank" : "Main assist";
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeGroupNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace group;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto error = [&events, &frameXml](const std::string& text) { raiseUiError(events, frameXml, text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };
    auto rosterEvents = [addon](bool raid, bool roles) {
        addon("GROUP_ROSTER_UPDATE");
        addon("PARTY_MEMBERS_CHANGED");
        // The roles came with this roster and UnitGroupRolesAssigned reads
        // them straight out of it. The player frame draws its tank or healer
        // badge on this event and nothing else.
        if (roles) addon("PLAYER_ROLES_ASSIGNED");
        if (raid) addon("RAID_ROSTER_UPDATE");
    };

    s.push_back(events.subscribe<JoinedGroup>([line](const JoinedGroup& e) {
        std::string names;
        for (const auto& n : e.members) names += (names.empty() ? "" : ", ") + n;
        line("You joined a group with: " + names);
        LOG_INFO("Joined group with ", e.members.size(), " members: ", names);
    }));
    s.push_back(events.subscribe<NoLongerInGroup>([line](const auto&) {
        line("You are no longer in a group.");
        LOG_INFO("Left group");
    }));
    s.push_back(events.subscribe<LootMethodChanged>([line, addon](const LootMethodChanged& e) {
        line(std::string("Loot method changed to ") + lootMethodName(e.method) + ".");
        // The party frames read the method to decide whether to offer the
        // master-looter menu, and they reread it on this.
        addon("PARTY_LOOT_METHOD_CHANGED");
    }));
    s.push_back(events.subscribe<RosterReplaced>([rosterEvents](const RosterReplaced& e) { rosterEvents(e.raid, true); }));
    s.push_back(events.subscribe<LeftGroup>([rosterEvents](const auto&) { rosterEvents(false, false); }));
    s.push_back(events.subscribe<GroupDisbanded>([line, error, rosterEvents](const auto&) {
        error("Your party has been disbanded.");
        line("Your party has been disbanded.");
        rosterEvents(false, false);
    }));
    s.push_back(events.subscribe<RemovedFromGroup>([line, error, rosterEvents](const auto&) {
        rosterEvents(true, false);
        error("You have been removed from the group.");
        line("You have been removed from the group.");
    }));

    s.push_back(events.subscribe<InviteReceived>([line, error, addon](const InviteReceived& e) {
        line(e.inviter + " has invited you to a group.");
        error(e.inviter + " has invited you to join a group.");
        addon("PARTY_INVITE_REQUEST", {e.inviter});
    }));
    // The server sends the invite with a zero in front to say "you were
    // invited and it could not be offered, because you are already in a
    // group" - a refusal, not an offer: no popup, no sound, no pending invite.
    s.push_back(events.subscribe<InviteBlockedAlreadyGrouped>([line](const InviteBlockedAlreadyGrouped& e) {
        line("|Hplayer:" + e.inviter + "|h[" + e.inviter + "]|h invited you to a group, but you "
             "could not accept because you are already in a group.");
    }));
    s.push_back(events.subscribe<InviteDeclined>([line](const InviteDeclined& e) {
        line(e.name + " has declined your group invitation.");
    }));
    s.push_back(events.subscribe<InviteCancelled>([line](const auto&) { line("Group invite cancelled."); }));
    s.push_back(events.subscribe<PartyCommandFailed>([line, error](const PartyCommandFailed& e) {
        const std::string msg = partyError(e);
        error(msg);
        line(msg);
    }));

    s.push_back(events.subscribe<MemberStatsChanged>([addon](const MemberStatsChanged& e) {
        if (!e.unit.empty()) {
            if (e.updateFlags & (StatsField::CurHealth | StatsField::MaxHealth)) addon("UNIT_HEALTH", {e.unit});
            if (e.updateFlags & (StatsField::CurPower | StatsField::MaxPower)) addon("UNIT_POWER", {e.unit});
            if (e.updateFlags & StatsField::Auras) addon("UNIT_AURA", {e.unit});
        }
        // Fired without arguments, as the retail client does - the interface
        // re-reads the whole roster rather than acting on one member. Not
        // gated on the unit: the player's own stats resolve to no unit token,
        // and a member dropping is worth announcing either way.
        if (e.onlineChanged) addon(e.online ? "PARTY_MEMBER_ENABLE" : "PARTY_MEMBER_DISABLE");
    }));
    s.push_back(events.subscribe<LeaderChanged>([line, addon](const LeaderChanged& e) {
        if (!e.name.empty()) line(e.name + " is now the group leader.");
        addon("PARTY_LEADER_CHANGED");
        addon("GROUP_ROSTER_UPDATE");
    }));
    s.push_back(events.subscribe<GroupReshaped>([addon](const auto&) {
        addon("PARTY_LEADER_CHANGED");
        addon("GROUP_ROSTER_UPDATE");
    }));

    s.push_back(events.subscribe<ReadyCheckStarted>([line, addon](const ReadyCheckStarted& e) {
        line(e.initiator.empty() ? "Ready check initiated!" : e.initiator + " initiated a ready check!");
        addon("READY_CHECK", {e.initiator});
    }));
    s.push_back(events.subscribe<ReadyCheckAnswered>([line, addon](const ReadyCheckAnswered& e) {
        if (!e.name.empty()) line(e.name + " is " + (e.ready ? "Ready" : "Not Ready") + ".");
        char guid[32];
        std::snprintf(guid, sizeof(guid), "0x%016llX", static_cast<unsigned long long>(e.guid));
        addon("READY_CHECK_CONFIRM", {guid, e.ready ? "1" : "0"});
    }));
    s.push_back(events.subscribe<ReadyCheckFinished>([line, addon](const ReadyCheckFinished& e) {
        line("Ready check complete: " + std::to_string(e.ready) + " ready, " +
             std::to_string(e.notReady) + " not ready.");
        addon("READY_CHECK_FINISHED");
    }));
    s.push_back(events.subscribe<RaidMarksChanged>([addon](const auto&) { addon("RAID_TARGET_UPDATE"); }));

    s.push_back(events.subscribe<UninviteSent>([line](const UninviteSent& e) {
        line("Removed " + e.name + " from the group.");
    }));
    s.push_back(events.subscribe<AssignmentSet>([line](const AssignmentSet& e) {
        line(std::string(assignmentName(e.assignment)) + " set.");
    }));
    s.push_back(events.subscribe<AssignmentCleared>([line](const AssignmentCleared& e) {
        line(std::string(assignmentName(e.assignment)) + " cleared.");
    }));
    s.push_back(events.subscribe<ReadyCheckSent>([line](const auto&) { line("Ready check initiated."); }));
    s.push_back(events.subscribe<ReadyCheckResponded>([line](const ReadyCheckResponded& e) {
        line(e.ready ? "You are ready." : "You are not ready.");
    }));
    s.push_back(events.subscribe<UninviteNeedsName>([error](const auto&) {
        error("You must specify a player name to uninvite.");
    }));
    s.push_back(events.subscribe<NotInGroup>([error](const auto&) { error("You are not in a group."); }));
    s.push_back(events.subscribe<NotGroupLeader>([error](const auto&) {
        error("You must be the party leader to convert to raid.");
    }));
    s.push_back(events.subscribe<AlreadyInRaid>([error](const auto&) { error("You are already in a raid group."); }));
    s.push_back(events.subscribe<NoTargetSelected>([error](const auto&) { error("You must have a target selected."); }));
    s.push_back(events.subscribe<ReadyCheckNeedsGroup>([error](const auto&) {
        error("You must be in a group to initiate a ready check.");
    }));
    return s;
}

} // namespace wowee::game

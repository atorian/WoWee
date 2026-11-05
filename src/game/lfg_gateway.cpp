#include "game/lfg_gateway.hpp"

#include "foundation/logger.hpp"
#include "game/chat_log.hpp"
#include "game/entity.hpp"
#include "game/frame_xml_events.hpp"
#include "game/handler_types.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "lfg/domain/events.hpp"
#include "network/packet.hpp"

#include <memory>
#include <set>

namespace wowee::game {

using lfg::JoinRefusal;
using lfg::TeleportRefusal;

bool LfgGateway::online() const {
    return sender_.characterInWorld();
}

bool LfgGateway::connected() const {
    return sender_.connected();
}

// The dungeon finder is Wrath's. Classic and TBC have no CMSG_LFG_JOIN at all -
// TBC's 0x35C is CMSG_LFG_SET_AUTOJOIN, an unrelated packet - so the logical
// opcodes are unmapped there and wireOpcode answers 0xFFFF. Sending that would
// put a packet the server has no handler for on the wire.

void LfgGateway::sendJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) {
    if (wireOpcode(Opcode::CMSG_LFG_JOIN) == 0xFFFF) return;
    sender_.send(LfgJoinPacket::build(dungeonIds, roles));
}

void LfgGateway::sendLeave() {
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_LEAVE);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt32(0); pkt.writeUInt32(0); pkt.writeUInt32(0);
    sender_.send(pkt);
}

void LfgGateway::sendSetRoles(uint8_t roles) {
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_SET_ROLES);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt8(roles);
    sender_.send(pkt);
}

void LfgGateway::sendProposalAnswer(uint32_t proposalId, bool accept) {
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_PROPOSAL_RESULT);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt32(proposalId); pkt.writeUInt8(accept ? 1 : 0);
    sender_.send(pkt);
}

void LfgGateway::sendTeleport(bool toLfgDungeon) {
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_TELEPORT);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt8(toLfgDungeon ? 0 : 1);
    sender_.send(pkt);
}

void LfgGateway::sendBootVote(bool vote) {
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_SET_BOOT_VOTE);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt8(vote ? 1 : 0);
    sender_.send(pkt);
}

// Payload-free requests. The dungeon finder's ready popup asks for both in its
// OnShow, so the rewards it is about to draw are the current ones.
void LfgGateway::sendPlayerLockInfoRequest() {
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFD_PLAYER_LOCK_INFO_REQUEST));
    sender_.send(pkt);
}

void LfgGateway::sendPartyLockInfoRequest() {
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFD_PARTY_LOCK_INFO_REQUEST));
    sender_.send(pkt);
}

std::string LfgGateway::dungeonName(uint32_t dungeonId) const {
    return world_.lfgDungeonName(dungeonId);
}

std::string LfgGateway::playerName(uint64_t guid) const {
    return world_.playerName(guid);
}

void LfgGateway::requestPlayerName(uint64_t guid) {
    world_.requestPlayerName(guid);
}

std::optional<std::string> LfgGateway::unitName(uint64_t guid) const {
    if (auto e = world_.entities().getEntity(guid))
        if (auto u = std::dynamic_pointer_cast<Unit>(e)) return u->getName();
    return std::nullopt;
}

// LFGJoinResult (WotLK 3.3.5a). A uint32 on the wire, so a uint32 here -
// narrowing it to a byte would wrap a large value into a case that means
// something else.
JoinRefusal lfgJoinRefusal(uint32_t result) {
    switch (result) {
        case 1:  return JoinRefusal::RoleCheckFailed;
        case 2:  return JoinRefusal::GroupFull;
        case 3:  return JoinRefusal::NoLfgObject;
        case 4:  return JoinRefusal::NoSlotsPlayer;
        case 5:  return JoinRefusal::NoSlotsParty;
        case 6:  return JoinRefusal::RequirementsNotMet;
        case 7:  return JoinRefusal::MixedRealms;
        case 8:  return JoinRefusal::MembersNotPresent;
        case 9:  return JoinRefusal::InfoTimeout;
        case 10: return JoinRefusal::InvalidSlot;
        case 11: return JoinRefusal::Deserter;
        case 12: return JoinRefusal::PartyDeserter;
        case 13: return JoinRefusal::RandomCooldown;
        case 14: return JoinRefusal::PartyRandomCooldown;
        case 15: return JoinRefusal::InternalError;
        case 16: return JoinRefusal::NoRole;
        default: return JoinRefusal::Other;
    }
}

TeleportRefusal lfgTeleportRefusal(uint32_t reason) {
    switch (reason) {
        case 0:  return TeleportRefusal::NotInGroup;
        case 1:  return TeleportRefusal::NotInDungeon;
        case 2:  return TeleportRefusal::SummonPending;
        case 3:  return TeleportRefusal::Dead;
        case 4:  return TeleportRefusal::Deserter;
        case 5:  return TeleportRefusal::RequirementsNotMet;
        default: return TeleportRefusal::Other;
    }
}

namespace {

const char* joinRefusalText(JoinRefusal reason) {
    switch (reason) {
        case JoinRefusal::RoleCheckFailed:     return "Role check failed.";
        case JoinRefusal::GroupFull:           return "No LFG slots available for your group.";
        case JoinRefusal::NoLfgObject:         return "No LFG object found.";
        case JoinRefusal::NoSlotsPlayer:       return "No slots available (player).";
        case JoinRefusal::NoSlotsParty:        return "No slots available (party).";
        case JoinRefusal::RequirementsNotMet:  return "Dungeon requirements not met by all members.";
        case JoinRefusal::MixedRealms:         return "Party members are from different realms.";
        case JoinRefusal::MembersNotPresent:   return "Not all members are present.";
        case JoinRefusal::InfoTimeout:         return "Get info timeout.";
        case JoinRefusal::InvalidSlot:         return "Invalid dungeon slot.";
        case JoinRefusal::Deserter:            return "You are marked as a deserter.";
        case JoinRefusal::PartyDeserter:       return "A party member is marked as a deserter.";
        case JoinRefusal::RandomCooldown:      return "You are on a random dungeon cooldown.";
        case JoinRefusal::PartyRandomCooldown: return "A party member is on a random dungeon cooldown.";
        case JoinRefusal::InternalError:       return "Cannot join dungeon finder.";
        case JoinRefusal::NoRole:              return "No spec/role available.";
        case JoinRefusal::Other:               break;
    }
    return "Cannot join dungeon finder.";
}

const char* teleportRefusalText(TeleportRefusal reason) {
    switch (reason) {
        case TeleportRefusal::NotInGroup:         return "You are not in a LFG group.";
        case TeleportRefusal::NotInDungeon:       return "You are not in the dungeon.";
        case TeleportRefusal::SummonPending:      return "You have a summon pending.";
        case TeleportRefusal::Dead:               return "You are dead.";
        case TeleportRefusal::Deserter:           return "You have Deserter.";
        case TeleportRefusal::RequirementsNotMet: return "You do not meet the requirements.";
        case TeleportRefusal::Other:              break;
    }
    return "Teleport to dungeon denied.";
}

/// What a queue update says, by 3.3.5's LfgUpdateType.
const char* queueNewsText(uint8_t updateType) {
    switch (updateType) {
        case 5:                  // JOIN_QUEUE
        case 6:  return "Dungeon Finder: You have joined the queue.";
        case 13: return "Dungeon Finder: Added to the queue.";
        case 8:  return "Dungeon Finder: Removed from the queue.";
        case 7:  return "Dungeon Finder: The role check failed.";
        case 4:  return "Dungeon Finder: The role check was cancelled.";
        case 9:  return "Dungeon Finder: The group did not form - back in the queue.";
        case 10: return "Dungeon Finder: Someone declined - back in the queue.";
        case 11: return "Dungeon Finder: A group has been found!";
        case 17: return "Dungeon Finder: Your group disbanded.";
        default: return "";
    }
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeLfgNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, SocialQueries& names) {
    using namespace lfg;
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

    // The queue state moved, or the attempt to join it ended either way: the
    // panel re-reads it, and the minimap's dungeon eye is reached from this
    // and from nothing else.
    s.push_back(events.subscribe<QueueUpdated>([addon](const auto&) { addon("LFG_UPDATE"); }));
    s.push_back(events.subscribe<JoinedQueue>([line, &names](const JoinedQueue& e) {
        const std::string name = names.lfgDungeonName(e.dungeonId);
        line(name.empty() ? "Dungeon Finder: Joined the queue."
                          : "Dungeon Finder: Joined the queue for " + name + ".");
    }));
    s.push_back(events.subscribe<JoinFailed>([failure](const JoinFailed& e) {
        failure(std::string("Dungeon Finder: ") + joinRefusalText(e.reason));
    }));
    s.push_back(events.subscribe<LockInfoReceived>([addon](const auto&) {
        addon("LFG_LOCK_INFO_RECEIVED");
        addon("LFG_UPDATE_RANDOM_INFO");
    }));
    s.push_back(events.subscribe<QueueStatusUpdated>([addon](const auto&) { addon("LFG_QUEUE_STATUS_UPDATE"); }));

    s.push_back(events.subscribe<ProposalOffered>([line, &names](const ProposalOffered& e) {
        const std::string name = names.lfgDungeonName(e.dungeonId);
        line(name.empty() ? "Dungeon Finder: A group has been found. Accept or decline."
                          : "Dungeon Finder: A group has been found for " + name + ". Accept or decline.");
    }));
    s.push_back(events.subscribe<ProposalFailed>([failure](const auto&) {
        failure("Dungeon Finder: Group proposal failed.");
    }));
    s.push_back(events.subscribe<ProposalSucceeded>([line, &names](const ProposalSucceeded& e) {
        const std::string name = names.lfgDungeonName(e.dungeonId);
        line(name.empty() ? "Dungeon Finder: Group found! Entering dungeon..."
                          : "Dungeon Finder: Group found for " + name + "! Entering dungeon...");
    }));
    // LFDFrame_OnEvent opens the dialog on SHOW and closes it on SUCCEEDED or
    // FAILED; UPDATE alone only refreshes one already on screen.
    s.push_back(events.subscribe<ProposalUpdated>([addon](const auto&) { addon("LFG_PROPOSAL_UPDATE"); }));
    s.push_back(events.subscribe<ProposalShown>([addon](const auto&) { addon("LFG_PROPOSAL_SHOW"); }));
    s.push_back(events.subscribe<ProposalEnded>([addon](const ProposalEnded& e) {
        addon(e.succeeded ? "LFG_PROPOSAL_SUCCEEDED" : "LFG_PROPOSAL_FAILED");
    }));

    s.push_back(events.subscribe<RoleCheckFailed>([failure](const auto&) {
        failure("Dungeon Finder: Role check failed - missing required role.");
    }));
    s.push_back(events.subscribe<RoleCheckStarted>([line](const auto&) {
        line("Dungeon Finder: Performing role check...");
    }));
    s.push_back(events.subscribe<RoleCheckUpdated>([addon](const auto&) { addon("LFG_ROLE_CHECK_UPDATE"); }));
    s.push_back(events.subscribe<RoleCheckPopup>([addon](const RoleCheckPopup& e) {
        addon(e.show ? "LFG_ROLE_CHECK_SHOW" : "LFG_ROLE_CHECK_HIDE");
    }));
    // The role-check window lists who has picked what as they pick it, and it
    // learns that from this event alone. Booleans through eventBool: "0" is a
    // number and true in Lua, so a false argument in front of a true one
    // cannot be spelled as text.
    s.push_back(events.subscribe<RoleChosen>([line, addon](const RoleChosen& e) {
        std::string roles;
        if (e.roles & 0x02) roles += "Tank ";
        if (e.roles & 0x04) roles += "Healer ";
        if (e.roles & 0x08) roles += "DPS ";
        if (roles.empty()) roles = "None";
        if (e.ready) line(e.playerName + " has chosen: " + roles);
        addon("LFG_ROLE_CHECK_ROLE_CHOSEN", {e.playerName, eventBool((e.roles & 0x02) != 0),
                                             eventBool((e.roles & 0x04) != 0), eventBool((e.roles & 0x08) != 0)});
    }));
    // Three frames draw the same three role checkboxes and each ticks them
    // from GetLFGRoles when this arrives.
    s.push_back(events.subscribe<RolesChanged>([addon](const auto&) { addon("LFG_ROLE_UPDATE"); }));

    s.push_back(events.subscribe<QueueNews>([line](const QueueNews& e) { line(queueNewsText(e.updateType)); }));
    // Said once per type, so it can be added.
    s.push_back(events.subscribe<UnknownQueueUpdate>(
        [said = std::make_shared<std::set<uint8_t>>()](const UnknownQueueUpdate& e) {
            if (said->insert(e.updateType).second) {
                LOG_WARNING("SMSG_LFG_UPDATE: unhandled update type ", static_cast<int>(e.updateType),
                            " (extraInfo=", e.extraInfo, ", queued=", e.queued,
                            ") - the queue state is left as it was");
            }
        }));
    // The reward toast. It carries no arguments - the frame reads the nine
    // values back through GetLFGCompletionReward - and FrameXML announces the
    // reward itself, so no chat line.
    s.push_back(events.subscribe<CompletionRewarded>([addon](const auto&) { addon("LFG_COMPLETION_REWARD"); }));
    s.push_back(events.subscribe<BootEnded>([line](const BootEnded& e) {
        line(e.passed ? "Dungeon Finder: Vote kick passed - member removed." : "Dungeon Finder: Vote kick failed.");
    }));
    // The vote-to-kick dialog, whose countdown starts on this.
    s.push_back(events.subscribe<BootUpdated>([addon](const auto&) { addon("LFG_BOOT_PROPOSAL_UPDATE"); }));
    s.push_back(events.subscribe<TeleportDenied>([line](const TeleportDenied& e) {
        line(std::string("Dungeon Finder: ") + teleportRefusalText(e.reason));
    }));
    s.push_back(events.subscribe<FinderDisabled>([line](const auto&) {
        line("The Dungeon Finder is currently disabled.");
    }));
    // lfgframe.lua concatenates the name, so it is never nil.
    s.push_back(events.subscribe<ContinueOffered>([line, addon, &names](const ContinueOffered& e) {
        std::string name = names.lfgDungeonName(e.dungeonId);
        if (name.empty()) name = "your dungeon";
        line("Dungeon Finder: You may continue " + name + ".");
        addon("LFG_OFFER_CONTINUE", {name, std::to_string(e.dungeonId), std::to_string(e.typeId)});
    }));
    return s;
}

} // namespace wowee::game

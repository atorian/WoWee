#include "game/reputation_gateway.hpp"

#include <cstdio>
#include <cstdlib>

#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/reputation_handler.hpp"
#include "game/world_packet_data.hpp"
#include "network/packet.hpp"
#include "game/game_data.hpp"
#include "pipeline/dbc_loader.hpp"
#include "reputation/domain/events.hpp"

namespace wowee::game {


std::vector<reputation::FactionRecord> FactionData::factions() {
    std::vector<reputation::FactionRecord> out;
    const auto dbc = data_.table("Faction.dbc", 1, "the reputation panel's factions");

    // Faction.dbc WotLK 3.3.5a field layout:
    //   0: ID
    //   1: ReputationListID  (-1 / 0xFFFFFFFF = no reputation tracking)
    //   2-5:  ReputationRaceMask[4]
    //   6-9:  ReputationClassMask[4]
    //  10-13: ReputationBase[4]
    //  14-17: ReputationFlags[4]
    //  18:    ParentFactionID
    //  19-20: SpilloverRateIn, SpilloverRateOut (floats)
    //  21-22: SpilloverMaxRankIn, SpilloverMaxRankOut
    //  23:    Name (English locale, string ref)
    constexpr uint32_t ID_FIELD      = 0;
    constexpr uint32_t REPLIST_FIELD = 1;
    constexpr uint32_t PARENT_FIELD  = 18;
    constexpr uint32_t NAME_FIELD    = 23;  // enUS name string

    const uint32_t fieldCount = dbc->getFieldCount();
    // Classic/TBC have fewer fields; fall back gracefully
    const bool hasRepListField = fieldCount > REPLIST_FIELD;
    // The parent chain is what groups the reputation panel into the headers
    // the original interface draws.
    const bool hasParentField = fieldCount > PARENT_FIELD;
    if (fieldCount <= NAME_FIELD) {
        LOG_WARNING("Faction.dbc: unexpected field count ", fieldCount);
        // Don't abort - still try to load names from a shorter layout
    }
    const uint32_t nameField = (fieldCount > NAME_FIELD) ? NAME_FIELD : 22u;

    size_t tracked = 0;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        reputation::FactionRecord r;
        r.id = dbc->getUInt32(i, ID_FIELD);
        if (r.id == 0) continue;
        if (fieldCount > nameField) r.name = dbc->getString(i, nameField);
        if (hasParentField) r.parentId = dbc->getUInt32(i, PARENT_FIELD);
        // The starting standing by race and class, fields 2 to 13 in every
        // expansion's Faction.dbc.
        if (fieldCount > 13) {
            r.hasBase = true;
            for (uint32_t f = 0; f < 12; ++f) r.base[f] = dbc->getInt32(i, 2 + f);
        }
        if (hasRepListField) {
            r.reputationIndex = dbc->getUInt32(i, REPLIST_FIELD);
            if (r.reputationIndex != reputation::kNoReputationIndex) ++tracked;
        }
        out.push_back(std::move(r));
    }
    LOG_INFO("Faction.dbc: loaded ", out.size(), " factions, ", tracked, " with reputation tracking");
    return out;
}

bool ReputationGateway::online() const {
    return sender_.characterInWorld();
}

uint8_t ReputationGateway::playerRace() const { return player_.playerRace(); }

uint8_t ReputationGateway::playerClass() const { return player_.playerClass(); }

void ReputationGateway::sendWatched(int32_t reputationIndex) {
    // CMSG_SET_WATCHED_FACTION: int32 repListId (-1 = unwatch)
    network::Packet pkt(wireOpcode(Opcode::CMSG_SET_WATCHED_FACTION));
    pkt.writeUInt32(static_cast<uint32_t>(reputationIndex));
    sender_.send(pkt);
    LOG_DEBUG("CMSG_SET_WATCHED_FACTION: repListId=", reputationIndex);
}

void ReputationGateway::sendAtWar(uint32_t reputationIndex, bool atWar) {
    // CMSG_SET_FACTION_ATWAR: uint32 repListId + uint8 flag
    network::Packet pkt(wireOpcode(Opcode::CMSG_SET_FACTION_ATWAR));
    pkt.writeUInt32(reputationIndex);
    pkt.writeUInt8(atWar ? 1u : 0u);
    sender_.send(pkt);
    LOG_DEBUG("CMSG_SET_FACTION_ATWAR: repListId=", reputationIndex, " atWar=", atWar);
}

void ReputationGateway::sendInactive(uint32_t reputationIndex, bool inactive) {
    // CMSG_SET_FACTION_INACTIVE: uint32 repListId + uint8 flag
    network::Packet pkt(wireOpcode(Opcode::CMSG_SET_FACTION_INACTIVE));
    pkt.writeUInt32(reputationIndex);
    pkt.writeUInt8(inactive ? 1u : 0u);
    sender_.send(pkt);
    LOG_DEBUG("CMSG_SET_FACTION_INACTIVE: repListId=", reputationIndex, " inactive=", inactive);
}

std::vector<core::EventBus::Subscription>
subscribeReputationNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                           ReputationQueries& hostility) {
    using namespace reputation;
    std::vector<core::EventBus::Subscription> s;
    s.push_back(events.subscribe<StandingChanged>([&chat, &frameXml](const StandingChanged& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Reputation with %s %s by %d.", e.factionName.c_str(),
                      e.delta > 0 ? "increased" : "decreased", std::abs(e.delta));
        chat.addSystemChatMessage(buf);
        frameXml.fireFrameXmlEvent("UPDATE_FACTION", {});
        chat.addLocalChatLine(ChatType::COMBAT_FACTION_CHANGE, buf);
    }));
    // Whether each faction with a standing is hostile is the at-war flag these
    // carry, so units judged before they arrived are judged again.
    s.push_back(events.subscribe<StandingsInitialized>([&hostility](const auto&) {
        hostility.refreshUnitHostility();
    }));
    s.push_back(events.subscribe<AtWarChanged>([&hostility](const auto&) { hostility.refreshUnitHostility(); }));
    return s;
}

} // namespace wowee::game

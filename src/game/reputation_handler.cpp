#include "game/reputation_handler.hpp"

#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"
#include "game/game_utils.hpp"
#include "game/opcodes.hpp"
#include "game/world_packet_data.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace wowee::game {

ReputationHandler::ReputationHandler(FrameXmlEvents& frameXml, core::EventBus& events,
                                     ChatLog& chat, ReputationQueries& queries)
    : frameXml_(frameXml), events_(events), chat_(chat), queries_(queries) {
    subscriptions_.push_back(events_.subscribe<events::CharacterLoggingIn>(
        [this](const events::CharacterLoggingIn&) {
            standing_ = {};
            events_.publish(events::FactionListChanged{});
        }));
}

void ReputationHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_INITIALIZE_FACTIONS] = [this](network::Packet& p) { handleInitializeFactions(p); };
    table[Opcode::SMSG_SET_FACTION_STANDING] = [this](network::Packet& p) { handleSetFactionStanding(p); };
    table[Opcode::SMSG_SET_FACTION_ATWAR] = [this](network::Packet& p) { handleSetFactionAtWar(p); };
    table[Opcode::SMSG_SET_FACTION_VISIBLE] = [this](network::Packet& p) { handleSetFactionVisible(p); };
}

bool ReputationHandler::setFlag(uint32_t repListId, uint8_t flag, bool on) {
    if (repListId >= standing_.factions.size()) return false;
    auto& flags = standing_.factions[repListId].flags;
    if (on) flags |= flag;
    else    flags &= static_cast<uint8_t>(~flag);
    return true;
}

void ReputationHandler::handleInitializeFactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    const uint32_t count = packet.readUInt32();
    const size_t needed = static_cast<size_t>(count) * 5;
    if (!packet.hasRemaining(needed)) {
        LOG_WARNING("SMSG_INITIALIZE_FACTIONS: ", count, " factions in ", packet.getRemainingSize(),
                    " bytes; the reputation stays as it was");
        packet.skipAll();
        return;
    }
    auto& factions = standing_.factions;
    factions.clear();
    factions.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FactionStanding fs{};
        fs.flags = packet.readUInt8();
        fs.standing = static_cast<int32_t>(packet.readUInt32());
        factions.push_back(fs);
    }
    // Also keyed by faction, which is how everything that draws a standing
    // asks for one. This packet is indexed by ReputationListID and the map is
    // indexed by faction id, and only SMSG_SET_FACTION_STANDING wrote to the
    // map - so a standing was known from login and unreadable until it next
    // moved. The reputation tab drew every bar empty at Neutral however much
    // the character had earned.
    //
    // And each made whole: the packet carries what has been earned, and the
    // standing is that on top of where the faction starts for this race and
    // class. See factionBaseReputation.
    for (size_t repListId = 0; repListId < factions.size(); ++repListId) {
        const uint32_t factionId = queries_.factionIdByRepListId(static_cast<uint32_t>(repListId));
        if (factionId == 0) continue;
        auto& standing = factions[repListId].standing;
        standing += queries_.factionBaseReputation(factionId);
        standing_.byFaction[factionId] = standing;
    }
    LOG_INFO("Reputation: ", factions.size(), " factions initialised, ", standing_.byFaction.size(),
             " resolved to a faction id");
    events_.publish(events::FactionListChanged{});
    // Whether each faction with a standing is hostile is the at-war flag this
    // packet carries, so units judged before it arrived are judged again.
    queries_.refreshUnitHostility();
}

void ReputationHandler::handleSetFactionStanding(network::Packet& packet) {
    // Expansion wire layouts differ before the shared count + entries payload:
    //   Classic: count
    //   TBC:     RAF bonus (float), count
    //   WotLK:   RAF bonus (float), showVisual (uint8), count
    // Treating WotLK's first float byte as showVisual shifted the entire packet,
    // producing bogus counts, faction names, standings, and chat messages.
    if (isActiveExpansion("wotlk")) {
        if (!packet.hasRemaining(9)) return;
        /*float rafBonus =*/ packet.readFloat();
        /*uint8_t showVisual =*/ packet.readUInt8();
    } else if (isActiveExpansion("tbc")) {
        if (!packet.hasRemaining(8)) return;
        /*float rafBonus =*/ packet.readFloat();
    } else if (!packet.hasRemaining(4)) {
        return;
    }

    uint32_t count = packet.readUInt32();
    count = std::min(count, 128u);
    for (uint32_t i = 0; i < count && packet.hasRemaining(8); ++i) {
        // The packet carries Faction.dbc ReputationListID, not the faction ID.
        const uint32_t repListId = packet.readUInt32();
        int32_t standing = static_cast<int32_t>(packet.readUInt32());

        const uint32_t factionId = queries_.factionIdByRepListId(repListId);
        if (factionId == 0) {
            LOG_WARNING("SMSG_SET_FACTION_STANDING: unknown repListId=", repListId,
                        " standing=", standing, "; that standing is not shown");
            continue;
        }
        // Earned only, as at login: the whole is this on top of the start.
        standing += queries_.factionBaseReputation(factionId);

        int32_t oldStanding = 0;
        // SMSG_INITIALIZE_FACTIONS is indexed by ReputationListID and supplies the
        // standing before incremental updates. Without using it, the first message
        // reports the character's entire accumulated reputation as the gained amount.
        if (repListId < standing_.factions.size()) {
            oldStanding = standing_.factions[repListId].standing;
            standing_.factions[repListId].standing = standing;
        } else if (auto it = standing_.byFaction.find(factionId); it != standing_.byFaction.end()) {
            oldStanding = it->second;
        }
        standing_.byFaction[factionId] = standing;
        const int32_t delta = standing - oldStanding;
        if (delta == 0) continue;

        const std::string name = queries_.factionName(factionId);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Reputation with %s %s by %d.",
                      name.c_str(), delta > 0 ? "increased" : "decreased", std::abs(delta));
        chat_.addSystemChatMessage(buf);
        standing_.watched = factionId;
        events_.publish(events::ReputationChanged{name, delta, standing});
        frameXml_.fireFrameXmlEvent("UPDATE_FACTION", {});
        chat_.addLocalChatLine(ChatType::COMBAT_FACTION_CHANGE, buf);
    }
}

void ReputationHandler::handleSetFactionAtWar(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    const uint32_t repListId = packet.readUInt32();
    const uint8_t setAtWar = packet.readUInt8();
    if (setFlag(repListId, kFactionFlagAtWar, setAtWar != 0)) queries_.refreshUnitHostility();
}

void ReputationHandler::handleSetFactionVisible(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    const uint32_t repListId = packet.readUInt32();
    const uint8_t visible = packet.readUInt8();
    // A faction met for the first time joins the list the panel draws.
    if (setFlag(repListId, kFactionFlagVisible, visible != 0)) {
        events_.publish(events::FactionListChanged{});
    }
}

} // namespace wowee::game

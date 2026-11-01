#include "game/reputation_handler.hpp"

#include "game/game_events.hpp"
#include "game/game_utils.hpp"
#include "game/opcodes.hpp"
#include "game/reputation_gateway.hpp"
#include "core/logger.hpp"
#include "reputation/domain/records.hpp"
#include "reputation/services/reputation.hpp"

#include <algorithm>
#include <utility>

namespace wowee::game {

ReputationHandler::ReputationHandler(FrameXmlEvents& frameXml, core::EventBus& events,
                                     ChatLog& chat, ReputationQueries& queries)
    : queries_(queries) {
    for (auto& subscription : subscribeReputationNotices(events, chat, frameXml, queries))
        subscriptions_.push_back(std::move(subscription));
}

// The next character's standings arrive with its login, and until they do the
// last one's at-war flags and watched faction are not this one's.
void ReputationHandler::beginCharacter() {
    if (auto* reputation = feed()) reputation->beginCharacter();
}

void ReputationHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_INITIALIZE_FACTIONS] = [this](network::Packet& p) { handleInitializeFactions(p); };
    table[Opcode::SMSG_SET_FACTION_STANDING] = [this](network::Packet& p) { handleSetFactionStanding(p); };
    table[Opcode::SMSG_SET_FACTION_ATWAR] = [this](network::Packet& p) { handleSetFactionAtWar(p); };
    table[Opcode::SMSG_SET_FACTION_VISIBLE] = [this](network::Packet& p) { handleSetFactionVisible(p); };
}

reputation::IReputationFeed* ReputationHandler::feed() {
    auto* feed = queries_.reputationFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("Reputation is not wired into this client; reputation packets are dropped");
    return feed;
}

void ReputationHandler::handleInitializeFactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("SMSG_INITIALIZE_FACTIONS too short: ", packet.getSize(),
                    " bytes; the reputation stays as it was");
        packet.skipAll();
        return;
    }
    const uint32_t count = packet.readUInt32();
    const size_t needed = static_cast<size_t>(count) * 5;
    if (!packet.hasRemaining(needed)) {
        LOG_WARNING("SMSG_INITIALIZE_FACTIONS: ", count, " factions in ", packet.getRemainingSize(),
                    " bytes; the reputation stays as it was");
        packet.skipAll();
        return;
    }
    std::vector<reputation::FactionStanding> factions;
    factions.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        reputation::FactionStanding fs{};
        fs.flags = packet.readUInt8();
        fs.standing = static_cast<int32_t>(packet.readUInt32());
        factions.push_back(fs);
    }
    if (auto* reputation = feed()) reputation->onInitialize(std::move(factions));
}

void ReputationHandler::handleSetFactionStanding(network::Packet& packet) {
    // Expansion wire layouts differ before the shared count + entries payload:
    //   Classic: count
    //   TBC:     RAF bonus (float), count
    //   WotLK:   RAF bonus (float), showVisual (uint8), count
    // Treating WotLK's first float byte as showVisual shifted the entire packet,
    // producing bogus counts, faction names, standings, and chat messages.
    const size_t header = isActiveExpansion("wotlk") ? 9 : isActiveExpansion("tbc") ? 8 : 4;
    if (!packet.hasRemaining(header)) {
        LOG_WARNING("SMSG_SET_FACTION_STANDING too short: ", packet.getSize(), " bytes, ", header,
                    " needed for its header; no standing changes");
        packet.skipAll();
        return;
    }
    if (header >= 8) /*float rafBonus =*/ packet.readFloat();
    if (header == 9) /*uint8_t showVisual =*/ packet.readUInt8();

    const uint32_t count = packet.readUInt32();
    if (count > 128u) {
        LOG_WARNING("SMSG_SET_FACTION_STANDING: ", count, " standings, more than the 128 a "
                    "character can have; the first 128 are read");
    }
    const uint32_t read = std::min(count, 128u);
    auto* reputation = feed();
    for (uint32_t i = 0; i < read; ++i) {
        if (!packet.hasRemaining(8)) {
            LOG_WARNING("SMSG_SET_FACTION_STANDING truncated: ", i, " of ", count,
                        " standings in the packet; the rest do not change");
            packet.skipAll();
            break;
        }
        // The packet carries Faction.dbc ReputationListID, not the faction ID,
        // and what has been earned rather than the whole standing.
        const uint32_t repListId = packet.readUInt32();
        const int32_t earned = static_cast<int32_t>(packet.readUInt32());
        if (reputation) reputation->onStanding(repListId, earned);
    }
}

void ReputationHandler::handleSetFactionAtWar(network::Packet& packet) {
    if (!packet.hasRemaining(5)) {
        LOG_WARNING("SMSG_SET_FACTION_ATWAR too short: ", packet.getSize(),
                    " bytes; the at-war flag does not change");
        packet.skipAll();
        return;
    }
    const uint32_t repListId = packet.readUInt32();
    const uint8_t setAtWar = packet.readUInt8();
    if (auto* reputation = feed()) reputation->onAtWar(repListId, setAtWar != 0);
}

void ReputationHandler::handleSetFactionVisible(network::Packet& packet) {
    if (!packet.hasRemaining(5)) {
        LOG_WARNING("SMSG_SET_FACTION_VISIBLE too short: ", packet.getSize(),
                    " bytes; the faction is not added to the list");
        packet.skipAll();
        return;
    }
    const uint32_t repListId = packet.readUInt32();
    const uint8_t visible = packet.readUInt8();
    // A faction met for the first time joins the list the panel draws.
    if (auto* reputation = feed()) reputation->onVisible(repListId, visible != 0);
}

} // namespace wowee::game

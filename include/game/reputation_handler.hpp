#pragma once

#include "core/event_bus.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::game {

class PacketSender;
class FrameXmlEvents;
class ChatLog;

/// A faction the character has a standing with, by ReputationListID - the
/// order SMSG_INITIALIZE_FACTIONS lists them in, which is not the faction id.
struct FactionStanding {
    uint8_t flags = 0;
    int32_t standing = 0;
};

// Faction.dbc ReputationFlags, as SMSG_INITIALIZE_FACTIONS carries them.
inline constexpr uint8_t kFactionFlagVisible        = 0x01;  ///< shown in the list
inline constexpr uint8_t kFactionFlagAtWar          = 0x02;
inline constexpr uint8_t kFactionFlagHidden         = 0x04;  ///< never shown
inline constexpr uint8_t kFactionFlagInvisibleForced = 0x08;
inline constexpr uint8_t kFactionFlagPeaceForced    = 0x10;
inline constexpr uint8_t kFactionFlagInactive       = 0x20;  ///< on the inactive list

/// What the reputation asks of the rest of the game: what Faction.dbc says
/// about a faction, and the units whose hostility follows from it.
class ReputationQueries {
public:
    virtual ~ReputationQueries() = default;
    /// The faction a ReputationListID names, or 0 when Faction.dbc has none.
    [[nodiscard]] virtual uint32_t factionIdByRepListId(uint32_t repListId) const = 0;
    /// Where this character's standing with a faction starts, by race and
    /// class; the packets carry only what has been earned on top of it.
    [[nodiscard]] virtual int32_t factionBaseReputation(uint32_t factionId) const = 0;
    [[nodiscard]] virtual std::string factionName(uint32_t factionId) const = 0;
    /// The at-war flags changed: units judged by them are judged again.
    virtual void refreshUnitHostility() = 0;
};

/// The character's reputation: the standing with each faction, which are at
/// war, visible or inactive, and the one being watched. Owns the four
/// opcodes that change it.
class ReputationHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    ReputationHandler(FrameXmlEvents& frameXml, core::EventBus& events, ChatLog& chat,
                      ReputationQueries& queries);

    void registerOpcodes(DispatchTable& table);

    /// By ReputationListID, as the server listed them at login.
    [[nodiscard]] const std::vector<FactionStanding>& factions() const { return standing_.factions; }
    /// The whole standing by faction id: what was earned on top of where the
    /// faction starts for this character.
    [[nodiscard]] const std::unordered_map<uint32_t, int32_t>& standings() const {
        return standing_.byFaction;
    }
    [[nodiscard]] bool hasFlag(uint32_t repListId, uint8_t flag) const {
        return repListId < standing_.factions.size() &&
               (standing_.factions[repListId].flags & flag) != 0;
    }
    /// Set or clear a flag the player changed here (at war, inactive), ahead of
    /// the server's word. False for a faction the character has no standing
    /// with.
    bool setFlag(uint32_t repListId, uint8_t flag, bool on);

    [[nodiscard]] uint32_t watchedFaction() const { return standing_.watched; }
    void setWatchedFaction(uint32_t factionId) { standing_.watched = factionId; }

private:
    void handleInitializeFactions(network::Packet& packet);
    void handleSetFactionStanding(network::Packet& packet);
    void handleSetFactionAtWar(network::Packet& packet);
    void handleSetFactionVisible(network::Packet& packet);

    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    ReputationQueries& queries_;

    /// The character's. Assigned whole when a character logs in
    /// (events::CharacterLoggingIn): the next one's arrives with its login,
    /// and until it does the last one's at-war flags and watched faction are
    /// not this one's.
    struct Standing {
        std::vector<FactionStanding> factions;
        std::unordered_map<uint32_t, int32_t> byFaction;
        /// The faction the reputation bar shows: the one the player chose,
        /// or the last one that moved.
        uint32_t watched = 0;
    };
    Standing standing_;

    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace wowee::game

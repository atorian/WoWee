#pragma once

#include "core/event_bus.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace wowee::reputation { class IReputationFeed; }

namespace wowee::game {

class FrameXmlEvents;
class ChatLog;

/// What the reputation's protocol asks of the rest of the game: where the
/// standings go, who the player is, and the units whose hostility follows from
/// the at-war flags.
class ReputationQueries {
public:
    virtual ~ReputationQueries() = default;
    /// The reputation, or null in a client with no game.
    [[nodiscard]] virtual reputation::IReputationFeed* reputationFeed() = 0;
    [[nodiscard]] virtual uint8_t playerRace() const = 0;
    [[nodiscard]] virtual uint8_t playerClass() const = 0;
    /// The at-war flags changed: units judged by them are judged again.
    virtual void refreshUnitHostility() = 0;
};

/// The four opcodes that change the character's reputation, decoded and handed
/// to the reputation context, and what the player is told of it.
class ReputationHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    ReputationHandler(FrameXmlEvents& frameXml, core::EventBus& events, ChatLog& chat,
                      ReputationQueries& queries);

    void registerOpcodes(DispatchTable& table);
    /// A character is being logged in: none of the last one's standings.
    void beginCharacter();

private:
    void handleInitializeFactions(network::Packet& packet);
    void handleSetFactionStanding(network::Packet& packet);
    void handleSetFactionAtWar(network::Packet& packet);
    void handleSetFactionVisible(network::Packet& packet);
    /// The feed, or null with a warning said once.
    reputation::IReputationFeed* feed();

    ReputationQueries& queries_;
    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace wowee::game

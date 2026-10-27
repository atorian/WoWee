#pragma once

#include "core/event_bus.hpp"
#include "reputation/services/ports.hpp"

#include <vector>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::game {

class PacketSender;
class ChatLog;
class FrameXmlEvents;
class ReputationQueries;

/// Faction.dbc, read through the asset manager.
class FactionData final : public reputation::IFactionData {
public:
    explicit FactionData(pipeline::AssetManager* assets) : assets_(assets) {}

    [[nodiscard]] bool ready() const override;
    std::vector<reputation::FactionRecord> factions() override;

private:
    pipeline::AssetManager* assets_;
};

/// Reputation's port: the player's race and class, and the requests over the
/// world connection.
class ReputationGateway final : public reputation::IReputationGateway {
public:
    ReputationGateway(PacketSender& sender, ReputationQueries& player) : sender_(sender), player_(player) {}

    [[nodiscard]] bool online() const override;
    [[nodiscard]] uint8_t playerRace() const override;
    [[nodiscard]] uint8_t playerClass() const override;
    void sendWatched(int32_t reputationIndex) override;
    void sendAtWar(uint32_t reputationIndex, bool atWar) override;
    void sendInactive(uint32_t reputationIndex, bool inactive) override;

private:
    PacketSender& sender_;
    ReputationQueries& player_;
};

/// What follows from the standings: the chat lines and UPDATE_FACTION when one
/// moves, in the order they had, and every unit's hostility judged again when
/// the standings or an at-war flag arrive.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeReputationNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                           ReputationQueries& hostility);

} // namespace wowee::game

#pragma once

#include "core/event_bus.hpp"
#include "reputation/services/ports.hpp"

#include <vector>

namespace wowee::game {

class GameData;
class PacketSender;
class ChatLog;
class FrameXmlEvents;
class ReputationQueries;

/// Faction.dbc, read through the game's tables: one that cannot be read
/// stops the game.
class FactionData final : public reputation::IFactionData {
public:
    explicit FactionData(const GameData& data) : data_(data) {}

    /// Always: the table is there whenever it is asked for, or the game stops.
    [[nodiscard]] bool ready() const override { return true; }
    std::vector<reputation::FactionRecord> factions() override;

private:
    const GameData& data_;
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

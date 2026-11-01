#pragma once

#include "battleground/domain/events.hpp"
#include "battleground/domain/records.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <variant>
#include <vector>

namespace wowee::battleground {

/// The battlegrounds this player is queued for or in, what the battlemaster
/// last listed, the scoreboard, and where everyone is on the map.
class Battlegrounds {
public:
    using Clock = std::chrono::steady_clock;

    std::vector<BattlegroundChange> statusReceived(const BattlefieldStatus& status, Clock::time_point now);
    void listReceived(AvailableBgInfo info);
    void positionsReceived(std::vector<BgPlayerPosition> positions) { positions_ = std::move(positions); }
    std::vector<BattlegroundChange> scoreboardReceived(BgScoreboardData scoreboard);

    /// The slot an invitation would be answered for: the named one if it is
    /// invited, or the first invited one for 0xFFFFFFFF.
    [[nodiscard]] std::optional<BgQueueSlot> invitation(uint32_t queueSlot) const;
    /// The slot a refusal would leave: any invitation, or a named slot that is
    /// queued or invited - the server takes the same refusal for both.
    [[nodiscard]] std::optional<BgQueueSlot> leavable(uint32_t queueSlot) const;
    /// The player went through the portal.
    void entering(const BgQueueSlot& slot);
    std::vector<BattlegroundChange> declined(const BgQueueSlot& slot);

    void joinRequested(uint32_t bgTypeId);
    /// The server answered a join, one way or the other.
    void joinAnswered() { joinPendingType_ = 0; }
    std::vector<BattlegroundChange> timePassed(float seconds);
    /// Whether the positions may be asked for again: the interface asks every
    /// frame the map is open, and once a second is plenty.
    bool positionsRequestDue(Clock::time_point now);
    /// A character is logging in: the last one's queues, listings, scoreboard,
    /// positions and unanswered join are not its own. When positions were last
    /// asked for is the connection's and stays.
    void beginCharacter();

    [[nodiscard]] bool hasPendingInvite() const;
    [[nodiscard]] const std::array<BgQueueSlot, 3>& queues() const { return queues_; }
    [[nodiscard]] const std::vector<AvailableBgInfo>& available() const { return available_; }
    /// The scoreboard, or null before one with players in it arrived.
    [[nodiscard]] const BgScoreboardData* scoreboard() const {
        return scoreboard_.players.empty() ? nullptr : &scoreboard_;
    }
    [[nodiscard]] const std::vector<BgPlayerPosition>& positions() const { return positions_; }

private:
    std::array<BgQueueSlot, 3> queues_{};
    std::vector<AvailableBgInfo> available_;
    BgScoreboardData scoreboard_;
    std::vector<BgPlayerPosition> positions_;
    Clock::time_point lastPositionRequest_{};
    uint32_t joinPendingType_ = 0;
    float joinWaitSec_ = 0.0f;
};

} // namespace wowee::battleground

#pragma once

#include "battleground/domain/records.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::battleground {

/// Battlegrounds as the interface and the addon layer use them.
class IBattlegrounds {
public:
    virtual ~IBattlegrounds() = default;

    // ---- Reading ----
    [[nodiscard]] virtual bool hasPendingBgInvite() const = 0;
    [[nodiscard]] virtual const std::array<BgQueueSlot, 3>& getBgQueues() const = 0;
    [[nodiscard]] virtual const std::vector<AvailableBgInfo>& getAvailableBgs() const = 0;
    /// The scoreboard, or null before one with players in it arrived.
    [[nodiscard]] virtual const BgScoreboardData* getBgScoreboard() const = 0;
    [[nodiscard]] virtual const std::vector<BgPlayerPosition>& getBgPlayerPositions() const = 0;

    // ---- Requests ----
    /// Answer an invitation; 0xFFFFFFFF means whichever is waiting.
    virtual void acceptBattlefield(uint32_t queueSlot = 0xFFFFFFFF) = 0;
    /// Turn an invitation down, or leave a queue still waiting.
    virtual void declineBattlefield(uint32_t queueSlot = 0xFFFFFFFF) = 0;
    /// Leave the battleground the player is standing in.
    virtual void leaveBattlefield() = 0;
    /// Ask which instances of one battleground are running.
    virtual void requestBattlefieldList(uint32_t bgTypeId) = 0;
    /// Report a player in this battleground as not participating.
    virtual void reportPvpAfk(uint64_t playerGuid) = 0;
    /// Queue at a battlemaster. An instance of zero means the first available.
    virtual void joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                                 uint32_t instanceId, bool asGroup) = 0;
    virtual void requestPvpLog() = 0;
    /// Ask for everyone's position on the battleground map. Throttled: the
    /// interface asks from WorldMapFrame_OnUpdate, every frame the map is open.
    virtual void requestBattlefieldPositions() = 0;
};

/// What the protocol layer tells the battlegrounds.
class IBattlegroundsFeed {
public:
    virtual ~IBattlegroundsFeed() = default;

    /// SMSG_BATTLEFIELD_STATUS, one queue slot of it.
    virtual void onStatus(const BattlefieldStatus& status) = 0;
    /// SMSG_BATTLEFIELD_LIST.
    virtual void onList(AvailableBgInfo info) = 0;
    /// MSG_BATTLEGROUND_PLAYER_POSITIONS.
    virtual void onPositions(std::vector<BgPlayerPosition> positions) = 0;
    /// MSG_PVP_LOG_DATA.
    virtual void onScoreboard(BgScoreboardData scoreboard) = 0;
    // SMSG_GROUP_JOINED_BATTLEGROUND, the answer to a join and most often a
    // refusal. Each of the three answers it.
    /// The group joined the queue for this battleground.
    virtual void onGroupQueued(uint32_t bgTypeId) = 0;
    /// The queue was refused; `member` is who could not join, when the server
    /// names them; `code` is the server's number.
    virtual void onJoinRefused(JoinRefusal reason, int32_t code, std::optional<uint64_t> member) = 0;
    /// Answered with nothing to say.
    virtual void onJoinAcknowledged() = 0;
    virtual void onPortDenied() = 0;
    virtual void onRemovedFromQueue() = 0;
    virtual void onJoinedQueue() = 0;
    virtual void onPlayerEntered(uint64_t guid) = 0;
    virtual void onPlayerLeft(uint64_t guid) = 0;
    virtual void onAfkReportResult(uint8_t result) = 0;
    virtual void timePassed(float seconds) = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::battleground

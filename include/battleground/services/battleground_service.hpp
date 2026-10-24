#pragma once

#include "battleground/domain/battlegrounds.hpp"
#include "battleground/services/battlegrounds.hpp"
#include "battleground/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::battleground {

class BattlegroundService final : public IBattlegrounds, public IBattlegroundsFeed {
public:
    BattlegroundService(IBattlegroundGateway& gateway, IBattlegroundWorld& world, core::EventBus& events);

    // IBattlegrounds: reading
    [[nodiscard]] bool hasPendingBgInvite() const override { return bgs_.hasPendingInvite(); }
    [[nodiscard]] const std::array<BgQueueSlot, 3>& getBgQueues() const override { return bgs_.queues(); }
    [[nodiscard]] const std::vector<AvailableBgInfo>& getAvailableBgs() const override { return bgs_.available(); }
    [[nodiscard]] const BgScoreboardData* getBgScoreboard() const override { return bgs_.scoreboard(); }
    [[nodiscard]] const std::vector<BgPlayerPosition>& getBgPlayerPositions() const override { return bgs_.positions(); }

    // IBattlegrounds: requests
    void acceptBattlefield(uint32_t queueSlot = 0xFFFFFFFF) override;
    void declineBattlefield(uint32_t queueSlot = 0xFFFFFFFF) override;
    void leaveBattlefield() override;
    void requestBattlefieldList(uint32_t bgTypeId) override;
    void reportPvpAfk(uint64_t playerGuid) override;
    void joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId, uint32_t instanceId, bool asGroup) override;
    void requestPvpLog() override;
    void requestBattlefieldPositions() override;

    // IBattlegroundsFeed
    void onStatus(const BattlefieldStatus& status) override;
    void onList(AvailableBgInfo info) override { bgs_.listReceived(std::move(info)); }
    void onPositions(std::vector<BgPlayerPosition> positions) override { bgs_.positionsReceived(std::move(positions)); }
    void onScoreboard(BgScoreboardData scoreboard) override { apply(bgs_.scoreboardReceived(std::move(scoreboard))); }
    void onGroupQueued(uint32_t bgTypeId) override;
    void onJoinRefused(JoinRefusal reason, int32_t code, std::optional<uint64_t> member) override;
    void onJoinAcknowledged() override { bgs_.joinAnswered(); }
    void onPortDenied() override;
    void onRemovedFromQueue() override;
    void onJoinedQueue() override;
    void onPlayerEntered(uint64_t guid) override;
    void onPlayerLeft(uint64_t guid) override;
    void onAfkReportResult(uint8_t result) override;
    void timePassed(float seconds) override { apply(bgs_.timePassed(seconds)); }
    void beginCharacter() override { bgs_.beginCharacter(); }

private:
    void apply(const std::vector<BattlegroundChange>& changes);

    IBattlegroundGateway& gateway_;
    IBattlegroundWorld& world_;
    core::EventBus& events_;
    Battlegrounds bgs_;
};

} // namespace wowee::battleground

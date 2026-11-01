#include "battleground/domain/battlegrounds.hpp"

#include <utility>

namespace wowee::battleground {

namespace {

const std::pair<uint32_t, const char*> kBgNames[] = {
    {1,"Alterac Valley"},{2,"Warsong Gulch"},{3,"Arathi Basin"},
    {4,"Nagrand Arena"},{5,"Blade's Edge Arena"},{6,"All Arenas"},
    {7,"Eye of the Storm"},{8,"Ruins of Lordaeron"},{9,"Strand of the Ancients"},
    {10,"Dalaran Sewers"},{11,"Ring of Valor"},{30,"Isle of Conquest"},{32,"Random Battleground"},
};

constexpr uint32_t kQueued = 1;
constexpr uint32_t kInvited = 2;
constexpr uint32_t kInProgress = 3;
constexpr uint32_t kAnySlot = 0xFFFFFFFF;
/// What the server gives an invitation that names no timeout.
constexpr uint32_t kDefaultInviteSeconds = 80;
constexpr float kJoinAnswerSeconds = 5.0f;

std::string queueName(uint32_t bgTypeId, uint8_t arenaType) {
    const std::string known = battlegroundName(bgTypeId);
    if (arenaType > 0) {
        std::string name = std::to_string(arenaType) + "v" + std::to_string(arenaType) + " Arena";
        if (!known.empty()) name += " (" + known + ")";
        return name;
    }
    return known.empty() ? "Battleground #" + std::to_string(bgTypeId) : known;
}

} // namespace

std::string battlegroundName(uint32_t bgTypeId) {
    for (const auto& kv : kBgNames)
        if (kv.first == bgTypeId) return kv.second;
    return {};
}

std::vector<BattlegroundChange> Battlegrounds::statusReceived(const BattlefieldStatus& status,
                                                              Clock::time_point now) {
    joinPendingType_ = 0;  // answered
    const uint32_t slot = status.queueSlot;
    const std::string name = queueName(status.bgTypeId, status.arenaType);
    const uint32_t avgWaitSec = status.avgWaitMs / 1000;
    const uint32_t waitedSec = status.timeInQueueMs / 1000;

    // The server restates every queue every thirty seconds or so, and at each
    // zone change and login; a line only when a queue actually moved, or the
    // same "Queued for X." spams the chat every tick.
    bool changed = true;
    if (slot < queues_.size()) {
        auto& q = queues_[slot];
        const bool wasInvite = q.statusId == kInvited;
        changed = q.statusId != status.statusId || q.bgTypeId != status.bgTypeId;
        q.queueSlot = slot;
        q.bgTypeId = status.bgTypeId;
        q.arenaType = status.arenaType;
        q.statusId = status.statusId;
        q.bgName = name;
        q.minLevel = status.minLevel;
        q.maxLevel = status.maxLevel;
        q.instanceId = status.instanceId;
        q.isRated = status.isRated;
        if (status.statusId == kQueued) {
            q.avgWaitTimeSec = avgWaitSec;
            q.timeInQueueSec = waitedSec;
            q.queueTimeReceivedAt = now;
        }
        // An invitation with no timeout in it still has to count down from
    // something, and eighty seconds is what the server gives one.
    if (status.statusId == kInvited && !wasInvite) {
            q.inviteTimeout = status.inviteTimeoutMs ? status.inviteTimeoutMs / 1000 : kDefaultInviteSeconds;
            q.inviteReceivedTime = now;
        }
    }

    std::vector<BattlegroundChange> changes;
    if (changed)
        changes.emplace_back(QueueStatusChanged{slot, status.statusId, status.bgTypeId, name, avgWaitSec, waitedSec});
    // The argument is which queue changed, counted from one, not what it
    // changed to. battlefieldframe.lua takes it as the index and raises the
    // "enter battleground" prompt only for the queue it names - handed the
    // status, an invitation (2) to the first queue named the second, and the
    // prompt never came up however long the player had waited for it.
    changes.emplace_back(QueueSlotUpdated{slot + 1});
    return changes;
}

void Battlegrounds::listReceived(AvailableBgInfo info) {
    for (auto& existing : available_) {
        if (existing.bgTypeId == info.bgTypeId) {
            existing = std::move(info);
            return;
        }
    }
    available_.push_back(std::move(info));
}

std::vector<BattlegroundChange> Battlegrounds::scoreboardReceived(BgScoreboardData scoreboard) {
    scoreboard_ = std::move(scoreboard);
    return {ScoreUpdated{}};
}

std::optional<BgQueueSlot> Battlegrounds::invitation(uint32_t queueSlot) const {
    if (queueSlot == kAnySlot) {
        for (const auto& q : queues_)
            if (q.statusId == kInvited) return q;
        return std::nullopt;
    }
    if (queueSlot < queues_.size() && queues_[queueSlot].statusId == kInvited) return queues_[queueSlot];
    return std::nullopt;
}

std::optional<BgQueueSlot> Battlegrounds::leavable(uint32_t queueSlot) const {
    if (queueSlot == kAnySlot) return invitation(kAnySlot);
    if (queueSlot < queues_.size() &&
        (queues_[queueSlot].statusId == kQueued || queues_[queueSlot].statusId == kInvited))
        return queues_[queueSlot];
    return std::nullopt;
}

void Battlegrounds::entering(const BgQueueSlot& slot) {
    if (slot.queueSlot < queues_.size()) queues_[slot.queueSlot].statusId = kInProgress;
}

std::vector<BattlegroundChange> Battlegrounds::declined(const BgQueueSlot& slot) {
    if (slot.queueSlot < queues_.size()) queues_[slot.queueSlot] = BgQueueSlot{};
    // The minimap icon and the panel read the queues on the update; without it
    // they kept showing the queue until the server's next word.
    return {QueueDeclined{slot.statusId == kInvited, slot.bgName}, QueueSlotUpdated{slot.queueSlot + 1}};
}

void Battlegrounds::joinRequested(uint32_t bgTypeId) {
    joinPendingType_ = bgTypeId;
    joinWaitSec_ = kJoinAnswerSeconds;
}

std::vector<BattlegroundChange> Battlegrounds::timePassed(float seconds) {
    if (joinPendingType_ == 0) return {};
    joinWaitSec_ -= seconds;
    if (joinWaitSec_ > 0.0f) return {};
    const uint32_t type = joinPendingType_;
    joinPendingType_ = 0;
    return {JoinUnanswered{type}};
}

bool Battlegrounds::positionsRequestDue(Clock::time_point now) {
    if (now - lastPositionRequest_ < std::chrono::seconds(1)) return false;
    lastPositionRequest_ = now;
    return true;
}

void Battlegrounds::beginCharacter() {
    queues_ = {};
    available_.clear();
    scoreboard_ = BgScoreboardData{};
    positions_.clear();
    joinPendingType_ = 0;
    joinWaitSec_ = 0.0f;
}

bool Battlegrounds::hasPendingInvite() const {
    for (const auto& q : queues_)
        if (q.statusId == kInvited) return true;
    return false;
}

} // namespace wowee::battleground

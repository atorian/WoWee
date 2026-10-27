#include "achievement/domain/achievements.hpp"

namespace wowee::achievement {

std::vector<AchievementChange> Achievements::replaceAll(const std::vector<EarnedAchievement>& earned,
                                                        const std::vector<CriteriaCounter>& criteria) {
    earned_.clear();
    dates_.clear();
    for (const auto& e : earned) {
        earned_.insert(e.id);
        dates_[e.id] = e.date;
    }
    criteria_.clear();
    for (const auto& c : criteria) criteria_[c.id] = c.counter;
    return {AchievementListReceived{}};
}

std::vector<AchievementChange> Achievements::earn(uint32_t id, uint32_t date) {
    earned_.insert(id);
    dates_[id] = date;
    return {AchievementEarned{id}};
}

void Achievements::revoke(uint32_t id) {
    earned_.erase(id);
    dates_.erase(id);
}

void Achievements::resetCriteria(uint32_t criteriaId) {
    criteria_.erase(criteriaId);
}

std::vector<AchievementChange> Achievements::progress(uint32_t criteriaId, uint64_t counter, uint32_t elapsed,
                                                      CriterionIndex where) {
    std::vector<AchievementChange> changes;
    auto it = criteria_.find(criteriaId);
    const uint64_t before = it != criteria_.end() ? it->second : 0;
    criteria_[criteriaId] = counter;
    if (counter != before) changes.emplace_back(CriteriaProgressed{criteriaId, counter});
    // Reported for any timed criteria rather than only tracked ones: tracking
    // an achievement part-way through its timer then shows the right
    // countdown, instead of nothing until the next update.
    if (where.timeLimit != 0 && where.achievementId != 0)
        changes.emplace_back(TimedCriteriaUpdated{where.achievementId, criteriaId, elapsed, where.timeLimit});
    return changes;
}

std::vector<AchievementChange> Achievements::inspected(uint64_t guid,
                                                       std::unordered_map<uint32_t, uint32_t> earned) {
    inspected_[guid] = std::move(earned);
    return {InspectAchievementsReady{}};
}

void Achievements::track(uint32_t id, bool tracked) {
    if (tracked) tracked_.insert(id); else tracked_.erase(id);
}

uint32_t Achievements::dateOf(uint32_t id) const {
    auto it = dates_.find(id);
    return it != dates_.end() ? it->second : 0u;
}

const std::unordered_map<uint32_t, uint32_t>* Achievements::inspectedOf(uint64_t guid) const {
    auto it = inspected_.find(guid);
    return it != inspected_.end() ? &it->second : nullptr;
}

} // namespace wowee::achievement

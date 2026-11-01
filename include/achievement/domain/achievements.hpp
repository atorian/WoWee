#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "achievement/domain/catalog.hpp"
#include "achievement/domain/events.hpp"
#include "achievement/domain/records.hpp"

namespace wowee::achievement {

/// The player's achievements as the server reports them, what the player is
/// watching, and what an inspected player has earned.
class Achievements {
public:
    /// SMSG_ALL_ACHIEVEMENT_DATA: the whole list replaces what was known.
    std::vector<AchievementChange> replaceAll(const std::vector<EarnedAchievement>& earned,
                                              const std::vector<CriteriaCounter>& criteria);
    std::vector<AchievementChange> earn(uint32_t id, uint32_t date);
    /// SMSG_ACHIEVEMENT_DELETED.
    void revoke(uint32_t id);
    /// SMSG_CRITERIA_DELETED.
    void resetCriteria(uint32_t criteriaId);
    /// A criteria's counter, and how far into its window the player is.
    /// `where` is what the DBC says about the criteria: a timed one drives the
    /// tracker's countdown whether or not the counter moved.
    std::vector<AchievementChange> progress(uint32_t criteriaId, uint64_t counter, uint32_t elapsed,
                                            CriterionIndex where);
    std::vector<AchievementChange> inspected(uint64_t guid, std::unordered_map<uint32_t, uint32_t> earned);
    /// A character is logging in: nothing earned, counted, tracked or
    /// inspected is theirs until the server says so.
    void beginCharacter() { *this = Achievements{}; }
    /// Client-side, like the quest tracker.
    void track(uint32_t id, bool tracked);
    void compareWith(uint64_t guid) { comparisonGuid_ = guid; }

    [[nodiscard]] const std::unordered_set<uint32_t>& earned() const { return earned_; }
    /// The packed time an achievement was earned, or zero.
    [[nodiscard]] uint32_t dateOf(uint32_t id) const;
    [[nodiscard]] const std::unordered_map<uint32_t, uint64_t>& criteria() const { return criteria_; }
    [[nodiscard]] const std::unordered_set<uint32_t>& tracked() const { return tracked_; }
    /// Achievement id to the packed date, or null when that player was never inspected.
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>* inspectedOf(uint64_t guid) const;
    [[nodiscard]] uint64_t comparisonGuid() const { return comparisonGuid_; }

private:
    std::unordered_set<uint32_t> earned_;
    std::unordered_map<uint32_t, uint32_t> dates_;
    std::unordered_map<uint32_t, uint64_t> criteria_;
    std::unordered_set<uint32_t> tracked_;
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> inspected_;
    uint64_t comparisonGuid_ = 0;
};

} // namespace wowee::achievement

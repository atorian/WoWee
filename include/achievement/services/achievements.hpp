#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::achievement {

struct CategoryInfo;
struct Criterion;
struct EarnedAchievement;
struct CriteriaCounter;

/// Achievements as the interface and the addon layer use them.
///
/// The reference data is read from the game's files on first ask: the names
/// with ensureAchievementNamesLoaded, the category tree with
/// ensureAchievementCategoriesLoaded and the criteria with
/// ensureAchievementCriteriaLoaded. Asking before the files can be read is not
/// an attempt; the next ask tries again.
class IAchievements {
public:
    virtual ~IAchievements() = default;

    [[nodiscard]] virtual const std::unordered_set<uint32_t>& getEarnedAchievements() const = 0;
    /// Criteria id to its counter.
    [[nodiscard]] virtual const std::unordered_map<uint32_t, uint64_t>& getCriteriaProgress() const = 0;
    /// The packed time an achievement was earned, or zero.
    [[nodiscard]] virtual uint32_t getAchievementDate(uint32_t id) const = 0;
    /// Points from every achievement the player has earned.
    [[nodiscard]] virtual uint32_t getTotalAchievementPoints() const = 0;
    [[nodiscard]] virtual const std::unordered_set<uint32_t>& getTrackedAchievements() const = 0;
    virtual void setAchievementTracked(uint32_t id, bool tracked) = 0;
    /// What an inspected player has earned: achievement id to the packed date.
    /// Null when nothing has arrived for that player.
    [[nodiscard]] virtual const std::unordered_map<uint32_t, uint32_t>* getInspectedPlayerAchievements(uint64_t guid) const = 0;
    /// Who the achievement comparison tab is looking at, or zero.
    [[nodiscard]] virtual uint64_t getAchievementComparisonGuid() const = 0;
    virtual void setAchievementComparisonGuid(uint64_t guid) = 0;

    virtual void ensureAchievementNamesLoaded() = 0;
    /// How many achievements Achievement.dbc names.
    [[nodiscard]] virtual size_t getAchievementCount() const = 0;
    /// Empty when unknown.
    [[nodiscard]] virtual const std::string& getAchievementName(uint32_t id) const = 0;
    [[nodiscard]] virtual const std::string& getAchievementDescription(uint32_t id) const = 0;
    [[nodiscard]] virtual uint32_t getAchievementPoints(uint32_t id) const = 0;
    /// Achievement.dbc's Flags column. FrameXML reads bit 0x80 to decide
    /// whether a row draws as a progress bar instead of a list of criteria.
    [[nodiscard]] virtual uint32_t getAchievementFlags(uint32_t id) const = 0;
    /// SpellIcon.dbc id of the achievement's icon, or zero.
    [[nodiscard]] virtual uint32_t getAchievementIconId(uint32_t id) const = 0;

    virtual void ensureAchievementCategoriesLoaded() = 0;
    /// Category ids in the order the panel shows them. The two tabs are
    /// disjoint sets, not one list filtered at the call site.
    [[nodiscard]] virtual const std::vector<uint32_t>& getAchievementCategoryOrder() const = 0;
    [[nodiscard]] virtual const std::vector<uint32_t>& getStatisticCategoryOrder() const = 0;
    [[nodiscard]] virtual const CategoryInfo* getAchievementCategoryInfo(uint32_t categoryId) const = 0;
    [[nodiscard]] virtual const std::vector<uint32_t>& getCategoryAchievements(uint32_t categoryId) const = 0;
    [[nodiscard]] virtual uint32_t getAchievementCategory(uint32_t achievementId) const = 0;
    /// The achievement this one supersedes, or zero.
    [[nodiscard]] virtual uint32_t getAchievementSupercedes(uint32_t achievementId) const = 0;
    [[nodiscard]] virtual uint32_t getAchievementSupercededBy(uint32_t achievementId) const = 0;

    virtual void ensureAchievementCriteriaLoaded() = 0;
    [[nodiscard]] virtual const std::unordered_map<uint32_t, std::vector<Criterion>>& getAchievementCriteriaMap() const = 0;
    [[nodiscard]] virtual const std::vector<Criterion>& getAchievementCriteria(uint32_t achievementId) const = 0;
};

/// `achievements`, or those of a client with no game: nothing earned, nothing
/// known and every change dropped - for a reader that would otherwise test
/// for it at every call.
[[nodiscard]] IAchievements& orNoAchievements(IAchievements* achievements);

/// What the protocol layer tells the achievements.
class IAchievementsFeed {
public:
    virtual ~IAchievementsFeed() = default;

    /// SMSG_ALL_ACHIEVEMENT_DATA.
    virtual void onAllData(const std::vector<EarnedAchievement>& earned,
                           const std::vector<CriteriaCounter>& criteria) = 0;
    /// SMSG_ACHIEVEMENT_EARNED, about the player.
    virtual void onEarned(uint32_t id, uint32_t date) = 0;
    /// SMSG_ACHIEVEMENT_DELETED.
    virtual void onRevoked(uint32_t id) = 0;
    /// SMSG_CRITERIA_DELETED.
    virtual void onCriteriaReset(uint32_t criteriaId) = 0;
    /// SMSG_CRITERIA_UPDATE: the counter as the panel shows it, and the
    /// seconds since the criteria's timer started.
    virtual void onCriteriaProgress(uint32_t criteriaId, uint64_t counter, uint32_t elapsed) = 0;
    /// SMSG_RESPOND_INSPECT_ACHIEVEMENTS.
    virtual void onInspected(uint64_t guid, std::unordered_map<uint32_t, uint32_t> earned) = 0;
    /// SMSG_SERVER_FIRST_ACHIEVEMENT.
    virtual void onServerFirst(const std::string& playerName, uint32_t achievementId) = 0;
    /// A character is logging in: none of the last one's achievements,
    /// counters, tracked set or inspect answers is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::achievement

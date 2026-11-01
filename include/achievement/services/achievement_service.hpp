#pragma once

#include "achievement/domain/achievements.hpp"
#include "achievement/domain/records.hpp"
#include "achievement/domain/catalog.hpp"
#include "achievement/services/achievements.hpp"
#include "achievement/services/ports.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::achievement {

class AchievementService final : public IAchievements, public IAchievementsFeed {
public:
    AchievementService(IAchievementData& data, core::EventBus& events);

    [[nodiscard]] const std::unordered_set<uint32_t>& getEarnedAchievements() const override { return player_.earned(); }
    [[nodiscard]] const std::unordered_map<uint32_t, uint64_t>& getCriteriaProgress() const override { return player_.criteria(); }
    [[nodiscard]] uint32_t getAchievementDate(uint32_t id) const override { return player_.dateOf(id); }
    [[nodiscard]] uint32_t getTotalAchievementPoints() const override;
    [[nodiscard]] const std::unordered_set<uint32_t>& getTrackedAchievements() const override { return player_.tracked(); }
    void setAchievementTracked(uint32_t id, bool tracked) override { player_.track(id, tracked); }
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>* getInspectedPlayerAchievements(uint64_t guid) const override {
        return player_.inspectedOf(guid);
    }
    [[nodiscard]] uint64_t getAchievementComparisonGuid() const override { return player_.comparisonGuid(); }
    void setAchievementComparisonGuid(uint64_t guid) override { player_.compareWith(guid); }

    void ensureAchievementNamesLoaded() override;
    [[nodiscard]] size_t getAchievementCount() const override { return catalog_.describedCount(); }
    [[nodiscard]] const std::string& getAchievementName(uint32_t id) const override { return catalog_.name(id); }
    [[nodiscard]] const std::string& getAchievementDescription(uint32_t id) const override { return catalog_.description(id); }
    [[nodiscard]] uint32_t getAchievementPoints(uint32_t id) const override { return catalog_.points(id); }
    [[nodiscard]] uint32_t getAchievementFlags(uint32_t id) const override { return catalog_.flags(id); }
    [[nodiscard]] uint32_t getAchievementIconId(uint32_t id) const override { return catalog_.iconId(id); }

    void ensureAchievementCategoriesLoaded() override;
    [[nodiscard]] const std::vector<uint32_t>& getAchievementCategoryOrder() const override { return catalog_.categoryOrder(); }
    [[nodiscard]] const std::vector<uint32_t>& getStatisticCategoryOrder() const override { return catalog_.statisticCategoryOrder(); }
    [[nodiscard]] const CategoryInfo* getAchievementCategoryInfo(uint32_t categoryId) const override { return catalog_.category(categoryId); }
    [[nodiscard]] const std::vector<uint32_t>& getCategoryAchievements(uint32_t categoryId) const override { return catalog_.achievementsIn(categoryId); }
    [[nodiscard]] uint32_t getAchievementCategory(uint32_t achievementId) const override { return catalog_.categoryOf(achievementId); }
    [[nodiscard]] uint32_t getAchievementSupercedes(uint32_t achievementId) const override { return catalog_.supercedes(achievementId); }
    [[nodiscard]] uint32_t getAchievementSupercededBy(uint32_t achievementId) const override { return catalog_.supercededBy(achievementId); }

    void ensureAchievementCriteriaLoaded() override;
    [[nodiscard]] const std::unordered_map<uint32_t, std::vector<Criterion>>& getAchievementCriteriaMap() const override { return catalog_.criteria(); }
    [[nodiscard]] const std::vector<Criterion>& getAchievementCriteria(uint32_t achievementId) const override { return catalog_.criteriaOf(achievementId); }

    void onAllData(const std::vector<EarnedAchievement>& earned, const std::vector<CriteriaCounter>& criteria) override;
    void onEarned(uint32_t id, uint32_t date) override;
    void onRevoked(uint32_t id) override { player_.revoke(id); }
    void onCriteriaReset(uint32_t criteriaId) override { player_.resetCriteria(criteriaId); }
    void onCriteriaProgress(uint32_t criteriaId, uint64_t counter, uint32_t elapsed) override;
    void onInspected(uint64_t guid, std::unordered_map<uint32_t, uint32_t> earned) override;
    void onServerFirst(const std::string& playerName, uint32_t achievementId) override;
    void beginCharacter() override { player_.beginCharacter(); }

private:
    void publish(const std::vector<AchievementChange>& changes);

    IAchievementData& data_;
    core::EventBus& events_;
    AchievementCatalog catalog_;
    Achievements player_;
    bool namesLoaded_ = false;
    bool categoriesLoaded_ = false;
    bool criteriaLoaded_ = false;
};

} // namespace wowee::achievement

#include "achievement/services/achievement_service.hpp"

#include "achievement/domain/events.hpp"
#include "foundation/event_bus.hpp"
#include "foundation/logger.hpp"

namespace wowee::achievement {

namespace {

class NoAchievements final : public IAchievements {
public:
    const std::unordered_set<uint32_t>& getEarnedAchievements() const override { return ids_; }
    const std::unordered_map<uint32_t, uint64_t>& getCriteriaProgress() const override { return progress_; }
    uint32_t getAchievementDate(uint32_t) const override { return 0; }
    uint32_t getTotalAchievementPoints() const override { return 0; }
    const std::unordered_set<uint32_t>& getTrackedAchievements() const override { return ids_; }
    void setAchievementTracked(uint32_t, bool) override {}
    const std::unordered_map<uint32_t, uint32_t>* getInspectedPlayerAchievements(uint64_t) const override { return nullptr; }
    uint64_t getAchievementComparisonGuid() const override { return 0; }
    void setAchievementComparisonGuid(uint64_t) override {}

    void ensureAchievementNamesLoaded() override {}
    size_t getAchievementCount() const override { return 0; }
    const std::string& getAchievementName(uint32_t) const override { return text_; }
    const std::string& getAchievementDescription(uint32_t) const override { return text_; }
    uint32_t getAchievementPoints(uint32_t) const override { return 0; }
    uint32_t getAchievementFlags(uint32_t) const override { return 0; }
    uint32_t getAchievementIconId(uint32_t) const override { return 0; }

    void ensureAchievementCategoriesLoaded() override {}
    const std::vector<uint32_t>& getAchievementCategoryOrder() const override { return list_; }
    const std::vector<uint32_t>& getStatisticCategoryOrder() const override { return list_; }
    const CategoryInfo* getAchievementCategoryInfo(uint32_t) const override { return nullptr; }
    const std::vector<uint32_t>& getCategoryAchievements(uint32_t) const override { return list_; }
    uint32_t getAchievementCategory(uint32_t) const override { return 0; }
    uint32_t getAchievementSupercedes(uint32_t) const override { return 0; }
    uint32_t getAchievementSupercededBy(uint32_t) const override { return 0; }

    void ensureAchievementCriteriaLoaded() override {}
    const std::unordered_map<uint32_t, std::vector<Criterion>>& getAchievementCriteriaMap() const override { return criteria_; }
    const std::vector<Criterion>& getAchievementCriteria(uint32_t) const override { return criterionList_; }

private:
    std::unordered_set<uint32_t> ids_;
    std::unordered_map<uint32_t, uint64_t> progress_;
    std::string text_;
    std::vector<uint32_t> list_;
    std::unordered_map<uint32_t, std::vector<Criterion>> criteria_;
    std::vector<Criterion> criterionList_;
};

NoAchievements kNoAchievements;

} // namespace

IAchievements& orNoAchievements(IAchievements* achievements) {
    return achievements ? *achievements : kNoAchievements;
}

AchievementService::AchievementService(IAchievementData& data, core::EventBus& events)
    : data_(data), events_(events) {}

uint32_t AchievementService::getTotalAchievementPoints() const {
    uint32_t total = 0;
    for (uint32_t id : player_.earned()) total += catalog_.points(id);
    return total;
}

// Each of the three is latched only once the files can be read: a caller can
// reach these before they can, and one early call would otherwise disable the
// file for the whole session.
void AchievementService::ensureAchievementNamesLoaded() {
    if (namesLoaded_ || !data_.ready()) return;
    namesLoaded_ = true;
    catalog_.describe(data_.achievements().rows);
}

void AchievementService::ensureAchievementCategoriesLoaded() {
    if (categoriesLoaded_ || !data_.ready()) return;
    ensureAchievementNamesLoaded();
    categoriesLoaded_ = true;
    const AchievementTable table = data_.achievements();
    if (!table.hasCategories) {
        if (!table.rows.empty())
            LOG_WARNING("Achievement.dbc has no category column - the achievement panel has no categories");
        return;
    }
    catalog_.place(table.rows, data_.categories());
}

void AchievementService::ensureAchievementCriteriaLoaded() {
    if (criteriaLoaded_ || !data_.ready()) return;
    criteriaLoaded_ = true;
    catalog_.addCriteria(data_.criteria());
}

void AchievementService::onAllData(const std::vector<EarnedAchievement>& earned,
                                   const std::vector<CriteriaCounter>& criteria) {
    ensureAchievementNamesLoaded();
    publish(player_.replaceAll(earned, criteria));
}

void AchievementService::onEarned(uint32_t id, uint32_t date) {
    ensureAchievementNamesLoaded();
    publish(player_.earn(id, date));
}

void AchievementService::onCriteriaProgress(uint32_t criteriaId, uint64_t counter, uint32_t elapsed) {
    ensureAchievementCriteriaLoaded();
    publish(player_.progress(criteriaId, counter, elapsed, catalog_.criterion(criteriaId)));
}

void AchievementService::onInspected(uint64_t guid, std::unordered_map<uint32_t, uint32_t> earned) {
    ensureAchievementNamesLoaded();
    publish(player_.inspected(guid, std::move(earned)));
}

void AchievementService::onServerFirst(const std::string& playerName, uint32_t achievementId) {
    ensureAchievementNamesLoaded();
    events_.publish(ServerFirstAchievement{playerName, achievementId});
}

void AchievementService::publish(const std::vector<AchievementChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

} // namespace wowee::achievement

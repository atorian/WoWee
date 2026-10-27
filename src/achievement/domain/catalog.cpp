#include "achievement/domain/catalog.hpp"

#include <algorithm>
#include <unordered_set>

namespace wowee::achievement {

namespace {
constexpr uint32_t kStatisticFlag = 0x1u;
}

void AchievementCatalog::describe(const std::vector<AchievementRecord>& rows) {
    for (const auto& r : rows) {
        if (r.id == 0) continue;
        if (!r.title.empty()) names_[r.id] = r.title;
        if (!r.description.empty()) descriptions_[r.id] = r.description;
        if (r.points > 0) points_[r.id] = r.points;
        if (r.iconId > 0) icons_[r.id] = r.iconId;
        if (r.flags != 0) flags_[r.id] = r.flags;
    }
}

void AchievementCatalog::placeAchievements(const std::vector<AchievementRecord>& rows) {
    for (const auto& r : rows) {
        if (r.id == 0 || names_.find(r.id) == names_.end()) continue;
        categoryOf_[r.id] = r.category;
        inCategory_[r.category].push_back(r.id);
        // Kept both ways round because the panel walks it backwards to list a
        // finished chain and forwards to find the step still open.
        if (r.supercedes != 0) {
            supercedes_[r.id] = r.supercedes;
            supercededBy_[r.supercedes] = r.id;
        }
    }
}

void AchievementCatalog::place(const std::vector<AchievementRecord>& rows,
                               const std::vector<CategoryRecord>& categories) {
    placeAchievements(rows);

    // Bit 0x1 of the flags marks a statistic rather than an achievement -
    // verified against all 1817 rows of the 3.3.5a file by the other thing that
    // says the same: whether the row's category descends from the top-level
    // Statistics category. No category holds a mixture, and the flag does not
    // depend on a category being named in English.
    std::unordered_set<uint32_t> statisticLeaves;
    for (const auto& [id, category] : categoryOf_)
        if ((flags(id) & kStatisticFlag) != 0) statisticLeaves.insert(category);

    for (const auto& c : categories) {
        CategoryInfo info;
        info.name = c.name;
        info.parentId = (c.parent == 0xFFFFFFFFu) ? -1 : static_cast<int32_t>(c.parent);
        info.uiOrder = c.uiOrder;
        categories_[c.id] = std::move(info);
        categoryOrder_.push_back(c.id);
    }

    // A category holding statistics is one, and so is every category above
    // it: the Statistics root holds no rows of its own, so marking only the
    // leaves would leave the tab with children and no trees to hang them on.
    for (uint32_t leaf : statisticLeaves) {
        for (int32_t c = static_cast<int32_t>(leaf); c >= 0; ) {
            auto it = categories_.find(static_cast<uint32_t>(c));
            if (it == categories_.end() || it->second.isStatistic) break;
            it->second.isStatistic = true;
            c = it->second.parentId;
        }
    }
    // Sorted by ui order, the top level comes out General, Quests, Exploration,
    // Player vs. Player, Dungeons & Raids, Professions, Reputation, World
    // Events, Feats of Strength, Statistics - the order the real client shows.
    // Id breaks ties, so the order is total and the same on every load:
    // siblings share a ui order across different parents.
    std::sort(categoryOrder_.begin(), categoryOrder_.end(), [this](uint32_t a, uint32_t b) {
        const uint32_t oa = categories_[a].uiOrder;
        const uint32_t ob = categories_[b].uiOrder;
        if (oa != ob) return oa < ob;
        return a < b;
    });
    // GetCategoryList and GetStatisticsCategoryList are separate in WoW
    // because a statistic is not a thing that can be earned.
    auto split = std::stable_partition(categoryOrder_.begin(), categoryOrder_.end(),
        [this](uint32_t c) { return !categories_[c].isStatistic; });
    statisticCategoryOrder_.assign(split, categoryOrder_.end());
    categoryOrder_.erase(split, categoryOrder_.end());
}

void AchievementCatalog::addCriteria(const std::vector<CriterionRecord>& rows) {
    for (const auto& r : rows) {
        if (r.achievementId == 0) continue;
        criterionById_[r.criterion.id] = {.achievementId = r.achievementId, .timeLimit = r.criterion.timeLimit};
        criteria_[r.achievementId].push_back(r.criterion);
    }
}

const std::string& AchievementCatalog::name(uint32_t id) const {
    static const std::string kEmpty;
    auto it = names_.find(id);
    return it != names_.end() ? it->second : kEmpty;
}

const std::string& AchievementCatalog::description(uint32_t id) const {
    static const std::string kEmpty;
    auto it = descriptions_.find(id);
    return it != descriptions_.end() ? it->second : kEmpty;
}

const CategoryInfo* AchievementCatalog::category(uint32_t categoryId) const {
    auto it = categories_.find(categoryId);
    return it != categories_.end() ? &it->second : nullptr;
}

const std::vector<uint32_t>& AchievementCatalog::achievementsIn(uint32_t categoryId) const {
    static const std::vector<uint32_t> kEmpty;
    auto it = inCategory_.find(categoryId);
    return it != inCategory_.end() ? it->second : kEmpty;
}

const std::vector<Criterion>& AchievementCatalog::criteriaOf(uint32_t achievementId) const {
    static const std::vector<Criterion> kEmpty;
    auto it = criteria_.find(achievementId);
    return it != criteria_.end() ? it->second : kEmpty;
}

CriterionIndex AchievementCatalog::criterion(uint32_t criteriaId) const {
    auto it = criterionById_.find(criteriaId);
    return it != criterionById_.end() ? it->second : CriterionIndex{};
}

} // namespace wowee::achievement

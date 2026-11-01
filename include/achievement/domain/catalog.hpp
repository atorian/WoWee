#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::achievement {

/// One Achievement.dbc row, as the reference data was read.
struct AchievementRecord {
    uint32_t    id = 0;
    std::string title;
    std::string description;
    uint32_t    points = 0;
    /// SpellIcon.dbc id of the achievement's artwork.
    uint32_t    iconId = 0;
    /// The Flags column. Bit 0x1 is ACHIEVEMENT_FLAGS_STATISTIC; bit 0x80 makes
    /// FrameXML draw the row as a progress bar.
    uint32_t    flags = 0;
    /// Achievement_Category.dbc id.
    uint32_t    category = 0;
    /// The achievement this one follows on from, or zero: "Level 20" names
    /// "Level 10", "Expert Cook" names "Journeyman Cook".
    uint32_t    supercedes = 0;
};

/// Achievement.dbc as it was read: the rows, and whether the file is wide
/// enough to say which category each row sits in.
struct AchievementTable {
    std::vector<AchievementRecord> rows;
    bool hasCategories = false;
};

/// One Achievement_Category.dbc row.
struct CategoryRecord {
    uint32_t    id = 0;
    /// -1 (0xFFFFFFFF) for a category at the top of the tree.
    uint32_t    parent = 0xFFFFFFFFu;
    std::string name;
    /// Where the category sits among its siblings.
    uint32_t    uiOrder = 0;
};

struct CategoryInfo {
    std::string name;
    int32_t     parentId = -1;  // -1 is a top-level category, as WoW reports it
    uint32_t    uiOrder  = 0;   // where it sits among its siblings
    bool        isStatistic = false;  // lives in the Statistics tree
};

struct Criterion {
    uint32_t    id = 0;
    std::string description;
    uint32_t    type = 0;
    uint32_t    assetId = 0;
    uint32_t    quantity = 0;
    /// Seconds the player has, once the criteria's timer starts. Zero for
    /// all but fifty-nine of the seven and a half thousand criteria - the
    /// ones whose own description names the limit, "Win Warsong Gulch in
    /// under 7 minutes" against 420 and "Kill Maexxna within 20 minutes"
    /// against 1200.
    uint32_t    timeLimit = 0;
    /// Bit one is ACHIEVEMENT_CRITERIA_PROGRESS_BAR, which is what makes the
    /// panel draw a bar instead of a tick - "Complete 2000 quests" carries it,
    /// "Axes" at four hundred does not, which is how WoW draws those two.
    uint32_t    flags = 0;
};

/// One Achievement_Criteria.dbc row: a criterion and the achievement it is part of.
struct CriterionRecord {
    uint32_t  achievementId = 0;
    Criterion criterion;
};

/// Where a criteria lives, by its own id. SMSG_CRITERIA_UPDATE names a
/// criteria and nothing else, and everything about it - which achievement
/// it belongs to, whether it is timed - is in the DBC under that id.
struct CriterionIndex {
    uint32_t achievementId = 0;
    uint32_t timeLimit = 0;
};

/// What the game's files say about achievements: names, points, the category
/// tree the panel draws and the criteria each achievement is made of.
class AchievementCatalog {
public:
    /// Titles, descriptions, points, icons and flags. A row with no title is
    /// not an achievement the panel can show.
    void describe(const std::vector<AchievementRecord>& rows);
    /// Puts each described achievement in its category, links the
    /// supercedes chains, and builds the category tree from `categories`.
    /// A category holding statistics is one, and so is every category above
    /// it; the two tabs get disjoint lists in the order the real client shows.
    void place(const std::vector<AchievementRecord>& rows, const std::vector<CategoryRecord>& categories);
    void addCriteria(const std::vector<CriterionRecord>& rows);

    [[nodiscard]] size_t describedCount() const { return names_.size(); }
    [[nodiscard]] const std::string& name(uint32_t id) const;
    [[nodiscard]] const std::string& description(uint32_t id) const;
    [[nodiscard]] uint32_t points(uint32_t id) const { return find(points_, id); }
    [[nodiscard]] uint32_t iconId(uint32_t id) const { return find(icons_, id); }
    [[nodiscard]] uint32_t flags(uint32_t id) const { return find(flags_, id); }

    [[nodiscard]] const std::vector<uint32_t>& categoryOrder() const { return categoryOrder_; }
    [[nodiscard]] const std::vector<uint32_t>& statisticCategoryOrder() const { return statisticCategoryOrder_; }
    [[nodiscard]] const CategoryInfo* category(uint32_t categoryId) const;
    [[nodiscard]] const std::vector<uint32_t>& achievementsIn(uint32_t categoryId) const;
    [[nodiscard]] uint32_t categoryOf(uint32_t achievementId) const { return find(categoryOf_, achievementId); }
    [[nodiscard]] uint32_t supercedes(uint32_t achievementId) const { return find(supercedes_, achievementId); }
    [[nodiscard]] uint32_t supercededBy(uint32_t achievementId) const { return find(supercededBy_, achievementId); }

    [[nodiscard]] const std::unordered_map<uint32_t, std::vector<Criterion>>& criteria() const { return criteria_; }
    [[nodiscard]] const std::vector<Criterion>& criteriaOf(uint32_t achievementId) const;
    [[nodiscard]] CriterionIndex criterion(uint32_t criteriaId) const;

private:
    void placeAchievements(const std::vector<AchievementRecord>& rows);
    static uint32_t find(const std::unordered_map<uint32_t, uint32_t>& map, uint32_t id) {
        auto it = map.find(id);
        return it != map.end() ? it->second : 0u;
    }

    std::unordered_map<uint32_t, std::string> names_;
    std::unordered_map<uint32_t, std::string> descriptions_;
    std::unordered_map<uint32_t, uint32_t> points_;
    std::unordered_map<uint32_t, uint32_t> icons_;
    std::unordered_map<uint32_t, uint32_t> flags_;

    std::unordered_map<uint32_t, uint32_t> categoryOf_;
    // Achievement ids per category, in the DBC's own order.
    std::unordered_map<uint32_t, std::vector<uint32_t>> inCategory_;
    std::unordered_map<uint32_t, CategoryInfo> categories_;
    std::vector<uint32_t> categoryOrder_;
    std::vector<uint32_t> statisticCategoryOrder_;
    std::unordered_map<uint32_t, uint32_t> supercedes_;
    std::unordered_map<uint32_t, uint32_t> supercededBy_;

    std::unordered_map<uint32_t, std::vector<Criterion>> criteria_;
    std::unordered_map<uint32_t, CriterionIndex> criterionById_;
};

} // namespace wowee::achievement

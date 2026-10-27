#pragma once

#include <vector>

#include "achievement/domain/catalog.hpp"

namespace wowee::achievement {

/// The game's files about achievements. Implemented over the asset manager.
class IAchievementData {
public:
    virtual ~IAchievementData() = default;

    /// Whether the files can be read yet.
    [[nodiscard]] virtual bool ready() const = 0;
    /// Achievement.dbc; no rows when it is missing.
    virtual AchievementTable achievements() = 0;
    /// Achievement_Category.dbc; nothing when it is missing.
    virtual std::vector<CategoryRecord> categories() = 0;
    /// Achievement_Criteria.dbc; nothing when it is missing.
    virtual std::vector<CriterionRecord> criteria() = 0;
};

} // namespace wowee::achievement

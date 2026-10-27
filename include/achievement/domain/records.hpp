#pragma once

#include <cstdint>

namespace wowee::achievement {

/// An achievement the player has, and the packed time it was earned.
struct EarnedAchievement {
    uint32_t id = 0;
    uint32_t date = 0;
};

/// How far one criteria has got.
struct CriteriaCounter {
    uint32_t id = 0;
    uint64_t counter = 0;
};

} // namespace wowee::achievement

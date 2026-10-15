#pragma once

#include <cstdint>
#include <string>

namespace wowee::game {

/// One row of a profession's recipe list.
///
/// Its own header because the interface keeps a view of these rows per
/// character (addons::InterfaceState) and has no other reason to include the
/// game handler.
struct CraftRecipe {
    uint32_t spellId = 0;
    std::string name;
    int difficulty = 0;   ///< 0 optimal, 1 medium, 2 easy, 3 trivial
    int canMake = 0;      ///< how many the bags have reagents for
};

} // namespace wowee::game

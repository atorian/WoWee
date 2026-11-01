#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::title {

/// The titles the player knows changed (KNOWN_TITLES_UPDATE).
struct KnownTitlesChanged {};

using TitleChange = std::variant<KnownTitlesChanged>;

// ---- Published by the service ----

/// SMSG_TITLE_EARNED: one title earned or taken away. `title` is the title
/// with the player's name in it, empty when CharTitles.dbc has no row for it.
struct TitleEarned {
    uint32_t bit = 0;
    bool lost = false;
    std::string title;
};

} // namespace wowee::title

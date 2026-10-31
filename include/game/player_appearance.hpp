#pragma once

#include "game/flat_field_map.hpp"

#include <cstdint>
#include <optional>

namespace wowee::game {

/// What a player looks like, packed as the character list packs it.
struct PlayerAppearance {
    uint8_t race = 0;
    uint8_t gender = 0;
    /// Skin, face, hair style and hair colour: PLAYER_BYTES as it is.
    uint32_t appearanceBytes = 0;
    /// Facial hair or markings: the low byte of PLAYER_BYTES_2.
    uint8_t facialFeatures = 0;
};

/// A player's look, read from their update fields: race and gender in
/// UNIT_FIELD_BYTES_0, the rest in PLAYER_BYTES and PLAYER_BYTES_2. A field
/// the map does not hold is zero, as it is on the server - a create block
/// leaves out every field that is zero, and the first skin, face and hair
/// are all zero. Null without UNIT_FIELD_BYTES_0: there is no race to draw.
[[nodiscard]] std::optional<PlayerAppearance> playerAppearance(const FlatFieldMap& fields);

} // namespace wowee::game

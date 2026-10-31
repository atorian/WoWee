#include "game/player_appearance.hpp"

#include "game/update_field_table.hpp"

namespace wowee::game {

std::optional<PlayerAppearance> playerAppearance(const FlatFieldMap& fields) {
    auto field = [&](UF name) -> std::optional<uint32_t> {
        const uint16_t idx = fieldIndex(name);
        if (idx == 0xFFFF) return std::nullopt;
        auto it = fields.find(idx);
        if (it == fields.end()) return std::nullopt;
        return it->second;
    };
    const auto bytes0 = field(UF::UNIT_FIELD_BYTES_0);
    if (!bytes0) return std::nullopt;
    return PlayerAppearance{
        .race = static_cast<uint8_t>(*bytes0 & 0xFF),
        .gender = static_cast<uint8_t>((*bytes0 >> 16) & 0xFF),
        .appearanceBytes = field(UF::PLAYER_BYTES).value_or(0),
        .facialFeatures = static_cast<uint8_t>(field(UF::PLAYER_BYTES_2).value_or(0) & 0xFF),
    };
}

} // namespace wowee::game

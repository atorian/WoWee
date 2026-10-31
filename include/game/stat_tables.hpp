#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game {

class GameData;

/// The combat game tables (gt*.dbc) the character sheet's derived numbers are
/// read from: crit from Agility and Intellect, regen from Spirit, and what a
/// combat rating is worth. Each table is read on its first ask. The sheet has
/// nothing true to show without them, so one that cannot be read, or is too
/// short for the row asked, stops the game.
///
/// Game data, like the emote table: the client owns one and hands it over
/// (GameServices::statTables).
class StatTables {
public:
    enum class Table {
        MeleeCritBase,          // gtChanceToMeleeCritBase: per class
        MeleeCritPerAgility,    // gtChanceToMeleeCrit: per class and level
        SpellCritBase,          // gtChanceToSpellCritBase: per class
        SpellCritPerIntellect,  // gtChanceToSpellCrit: per class and level
        OctRegenHp,             // gtOCTRegenHP: per class and level
        RegenHpPerSpirit,       // gtRegenHPPerSpt: per class and level
        RegenMpPerSpirit,       // gtRegenMPPerSpt: per class and level
        CombatRatings,          // gtCombatRatings: per rating and level
        ClassRatingScalar,      // gtOCTClassCombatRatingScalar: per class and rating
        Count
    };
    /// The levels a per-level table holds, and the ratings a per-rating one.
    static constexpr uint32_t kMaxLevel = 100;
    static constexpr uint32_t kRatings = 32;

    /// One column of a DBC as floats, its rows in order. Returns the table or
    /// does not return: a table that cannot be read stops the game in it.
    using ReadColumn = std::function<std::vector<float>(const std::string& dbc, uint32_t column)>;

    explicit StatTables(ReadColumn read);
    /// Read from the game's tables, which outlive this.
    static ReadColumn fromData(const GameData& data);

    /// A table's row for a class (1-11).
    [[nodiscard]] float perClass(Table table, uint8_t classId) const;
    /// A table's row for a class (1-11) at a level (1-100).
    [[nodiscard]] float perClassAndLevel(Table table, uint8_t classId, uint32_t level) const;
    /// gtCombatRatings: the rating's coefficient at a level (1-100).
    [[nodiscard]] float ratingAtLevel(uint32_t rating, uint32_t level) const;
    /// gtOCTClassCombatRatingScalar: the class's scalar for a rating.
    [[nodiscard]] float classRatingScalar(uint8_t classId, uint32_t rating) const;

private:
    [[nodiscard]] float row(Table table, size_t index) const;

    ReadColumn read_;
    mutable std::array<std::optional<std::vector<float>>, static_cast<size_t>(Table::Count)> tables_;
};

} // namespace wowee::game

#pragma once

/// What an item tooltip reads from the expansion's tables: skill, faction and
/// enchantment names and the item sets.
///
/// The bag tooltip, the tooltip of an item seen only through a query, and the
/// chat link's tooltip each read these tables themselves, three copies with
/// their own column guesses - the faction name came from column 20, which no
/// expansion's Faction.dbc keeps it in. Each is read here once, through the
/// expansion's layout, and read again only when the table it came from is
/// replaced (an expansion change).

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee::pipeline { class GameTables; }

namespace wowee::ui {

/// SkillLine.dbc: skill id -> name.
const std::unordered_map<uint32_t, std::string>& skillLineNames(const pipeline::GameTables& tables);

/// Faction.dbc: faction id -> name.
const std::unordered_map<uint32_t, std::string>& factionNames(const pipeline::GameTables& tables);

/// SpellItemEnchantment.dbc: enchantment id -> name.
const std::unordered_map<uint32_t, std::string>& enchantmentNames(const pipeline::GameTables& tables);

/// One row of ItemSet.dbc: its name, its pieces, and each bonus spell with the
/// number of pieces it needs.
struct ItemSetData {
    std::string name;
    std::array<uint32_t, 10> itemIds{};
    std::array<uint32_t, 10> spellIds{};
    std::array<uint32_t, 10> thresholds{};
};

/// ItemSet.dbc by set id. Empty for an expansion that does not use the
/// table, whose tooltips show no set.
const std::unordered_map<uint32_t, ItemSetData>& itemSets(const pipeline::GameTables& tables);

}  // namespace wowee::ui

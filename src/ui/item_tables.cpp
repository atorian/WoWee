#include "ui/item_tables.hpp"

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"

#include <utility>

namespace wowee::ui {

namespace {

/// ItemSet.dbc's columns as dbc_layouts.json names them.
constexpr const char* kItemSetItemKeys[10] = {
    "Item0", "Item1", "Item2", "Item3", "Item4", "Item5", "Item6", "Item7", "Item8", "Item9"};
constexpr const char* kItemSetSpellKeys[10] = {
    "Spell0", "Spell1", "Spell2", "Spell3", "Spell4", "Spell5", "Spell6", "Spell7", "Spell8", "Spell9"};
constexpr const char* kItemSetThresholdKeys[10] = {
    "Threshold0", "Threshold1", "Threshold2", "Threshold3", "Threshold4",
    "Threshold5", "Threshold6", "Threshold7", "Threshold8", "Threshold9"};

/// What was read from one table, kept until that table is replaced.
template <class Value>
struct Cached {
    pipeline::GameTables::Table source;
    std::unordered_map<uint32_t, Value> rows;
};

/// id -> the string in `field`, rows with no id or no string left out.
const std::unordered_map<uint32_t, std::string>& names(Cached<std::string>& cache,
                                                       const pipeline::RequiredTable& dbc,
                                                       uint32_t idField, uint32_t nameField) {
    if (cache.source == dbc.shared()) return cache.rows;
    cache.source = dbc.shared();
    cache.rows.clear();
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        const uint32_t id = dbc->getUInt32(r, idField);
        if (id == 0) continue;
        std::string name = dbc->getString(r, nameField);
        if (!name.empty()) cache.rows[id] = std::move(name);
    }
    return cache.rows;
}

}  // namespace

const std::unordered_map<uint32_t, std::string>& skillLineNames(const pipeline::GameTables& tables) {
    static Cached<std::string> cache;
    const pipeline::RequiredTable dbc(tables, "SkillLine.dbc", "the skill an item's tooltip requires");
    return names(cache, dbc, dbc.column("ID"), dbc.column("Name"));
}

const std::unordered_map<uint32_t, std::string>& factionNames(const pipeline::GameTables& tables) {
    static Cached<std::string> cache;
    const pipeline::RequiredTable dbc(tables, "Faction.dbc", "the reputation an item's tooltip requires");
    return names(cache, dbc, dbc.column("ID"), dbc.column("Name"));
}

const std::unordered_map<uint32_t, std::string>& enchantmentNames(const pipeline::GameTables& tables) {
    static Cached<std::string> cache;
    const pipeline::RequiredTable dbc(tables, "SpellItemEnchantment.dbc", "the enchantments in item tooltips");
    // From the file's shape: the record grew across expansions and the name
    // moved with it (detectEnchantmentNameField).
    return names(cache, dbc, dbc.column("ID"), pipeline::detectEnchantmentNameField(dbc.get(), &dbc.layout()));
}

const std::unordered_map<uint32_t, ItemSetData>& itemSets(const pipeline::GameTables& tables) {
    static Cached<ItemSetData> cache;
    if (!tables.has("ItemSet.dbc")) {
        cache = {};
        return cache.rows;
    }
    const pipeline::RequiredTable dbc(tables, "ItemSet.dbc", "the item sets in item tooltips");
    if (cache.source == dbc.shared()) return cache.rows;
    cache.source = dbc.shared();
    cache.rows.clear();
    const uint32_t idField = dbc.column("ID");
    const uint32_t nameField = dbc.column("Name");
    std::array<uint32_t, 10> itemFields{}, spellFields{}, thresholdFields{};
    for (size_t i = 0; i < 10; ++i) {
        itemFields[i] = dbc.column(kItemSetItemKeys[i]);
        spellFields[i] = dbc.column(kItemSetSpellKeys[i]);
        thresholdFields[i] = dbc.column(kItemSetThresholdKeys[i]);
    }
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        const uint32_t id = dbc->getUInt32(r, idField);
        if (id == 0) continue;
        ItemSetData set;
        set.name = dbc->getString(r, nameField);
        for (size_t i = 0; i < 10; ++i) {
            set.itemIds[i] = dbc->getUInt32(r, itemFields[i]);
            set.spellIds[i] = dbc->getUInt32(r, spellFields[i]);
            set.thresholds[i] = dbc->getUInt32(r, thresholdFields[i]);
        }
        cache.rows[id] = std::move(set);
    }
    return cache.rows;
}

}  // namespace wowee::ui

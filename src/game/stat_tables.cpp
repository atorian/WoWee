#include "game/stat_tables.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cstdlib>
#include <utility>

namespace wowee::game {

namespace {

struct TableSource {
    const char* dbc;
    uint32_t column;
};

// Single-column float tables, apart from the class scalar, which is id then
// ratio.
constexpr TableSource kSources[] = {
    {"gtChanceToMeleeCritBase.dbc", 0},
    {"gtChanceToMeleeCrit.dbc", 0},
    {"gtChanceToSpellCritBase.dbc", 0},
    {"gtChanceToSpellCrit.dbc", 0},
    {"gtOCTRegenHP.dbc", 0},
    {"gtRegenHPPerSpt.dbc", 0},
    {"gtRegenMPPerSpt.dbc", 0},
    {"gtCombatRatings.dbc", 0},
    {"gtOCTClassCombatRatingScalar.dbc", 1},
};
static_assert(std::size(kSources) == static_cast<size_t>(StatTables::Table::Count));

[[noreturn]] void stop(const std::string& why) {
    LOG_FATAL("Character sheet: ", why, "; the game cannot go on without it");
    std::abort();
}

}  // namespace

StatTables::StatTables(ReadColumn read) : read_(std::move(read)) {}

StatTables::ReadColumn StatTables::fromAssets(pipeline::AssetManager* assets) {
    return [assets](const std::string& dbc, uint32_t column) {
        if (!assets || !assets->isInitialized()) stop(dbc + " asked for with no game data loaded");
        auto table = assets->loadDBC(dbc);
        if (!table || !table->isLoaded()) stop(dbc + " could not be read");
        if (table->getRecordCount() == 0 || table->getFieldCount() <= column) {
            stop(dbc + " has " + std::to_string(table->getRecordCount()) + " rows of " +
                 std::to_string(table->getFieldCount()) + " fields, no column " +
                 std::to_string(column));
        }
        std::vector<float> rows;
        rows.reserve(table->getRecordCount());
        for (uint32_t i = 0; i < table->getRecordCount(); ++i) rows.push_back(table->getFloat(i, column));
        return rows;
    };
}

float StatTables::row(Table table, size_t index) const {
    const size_t t = static_cast<size_t>(table);
    if (!tables_[t]) tables_[t] = read_(kSources[t].dbc, kSources[t].column);
    const auto& rows = *tables_[t];
    if (index >= rows.size()) {
        stop(std::string(kSources[t].dbc) + " has " + std::to_string(rows.size()) +
             " rows; row " + std::to_string(index) + " was asked for");
    }
    return rows[index];
}

float StatTables::perClass(Table table, uint8_t classId) const {
    return row(table, static_cast<size_t>(classId) - 1);
}

float StatTables::perClassAndLevel(Table table, uint8_t classId, uint32_t level) const {
    return row(table, (static_cast<size_t>(classId) - 1) * kMaxLevel + (level - 1));
}

float StatTables::ratingAtLevel(uint32_t rating, uint32_t level) const {
    return row(Table::CombatRatings, static_cast<size_t>(rating) * kMaxLevel + (level - 1));
}

float StatTables::classRatingScalar(uint8_t classId, uint32_t rating) const {
    return row(Table::ClassRatingScalar, (static_cast<size_t>(classId) - 1) * kRatings + rating);
}

} // namespace wowee::game

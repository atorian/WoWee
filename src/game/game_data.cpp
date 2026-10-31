#include "game/game_data.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cstdlib>
#include <utility>

namespace wowee::game {

GameData::GameData(Read read) : read_(std::move(read)) {}

GameData::Read GameData::fromAssets(pipeline::AssetManager* assets) {
    // The asset manager says why when it cannot load one; the line that stops
    // the game says what needed it.
    return [assets](const std::string& dbc) -> Table {
        if (!assets || !assets->isInitialized()) return nullptr;
        return assets->loadDBC(dbc);
    };
}

GameData::Table GameData::table(const std::string& dbc, uint32_t fields,
                                std::string_view neededFor) const {
    Table table = read_(dbc);
    if (!table || !table->isLoaded()) {
        stop(dbc, neededFor, "could not be read");
    }
    if (table->getFieldCount() < fields) {
        stop(dbc, neededFor, "has " + std::to_string(table->getFieldCount()) + " fields, " +
                             std::to_string(fields) + " are read");
    }
    return table;
}

void GameData::stop(const std::string& dbc, std::string_view neededFor, const std::string& why) {
    LOG_FATAL(dbc, " ", why, "; ", neededFor, " needs it, and the game cannot go on without it");
    std::abort();
}

} // namespace wowee::game

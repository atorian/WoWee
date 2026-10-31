#include "game/game_data.hpp"

#include "core/logger.hpp"
#include "game/expansion_profile.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace wowee::game {

namespace {

constexpr std::string_view kCheck = "the expansion's game data, checked when it is opened";

bool listed(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

} // namespace

GameData::GameData(Read read, const ExpansionRegistry& expansions, Stop stop)
    : read_(std::move(read)), expansions_(expansions), stop_(std::move(stop)) {}

GameData::Read GameData::fromAssets(pipeline::AssetManager* assets) {
    // The asset manager says why when it cannot load one; the line that stops
    // the game says what needed it.
    return [assets](const std::string& dbc) -> Table {
        if (!assets || !assets->isInitialized()) return nullptr;
        return assets->loadDBC(dbc);
    };
}

const ExpansionProfile& GameData::expansion() const {
    const ExpansionProfile* profile = expansions_.getActive();
    if (!profile) stop("Data/expansions", "the game", "has no expansion running");
    return *profile;
}

bool GameData::has(const std::string& dbc) const {
    return listed(expansion().tables, dbc);
}

GameData::Table GameData::table(const std::string& dbc, uint32_t fields,
                                std::string_view neededFor) const {
    if (!has(dbc)) stop(dbc, neededFor, "is not on the expansion's list of tables it uses");
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

std::string GameData::file(const std::string& name, std::string_view neededFor) const {
    const ExpansionProfile& profile = expansion();
    if (!listed(profile.files, name)) stop(name, neededFor, "is not on the expansion's list of files it uses");
    const std::string path = profile.dataPath + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || !std::ifstream(path)) {
        stop(path, neededFor, "is not there or cannot be opened");
    }
    return path;
}

void GameData::checkAll() const {
    const ExpansionProfile& profile = expansion();
    if (profile.files.empty() || profile.tables.empty()) {
        stop(profile.dataPath + "/expansion.json", kCheck, "lists no files or no tables");
    }
    for (const std::string& name : profile.files) (void)file(name, kCheck);
    for (const std::string& dbc : profile.tables) (void)table(dbc, 1, kCheck);
    LOG_INFO("Game data: ", profile.files.size(), " files and ", profile.tables.size(),
             " tables of ", profile.name, " read");
}

void GameData::stop(const std::string& what, std::string_view neededFor, const std::string& why) const {
    const ExpansionProfile* profile = expansions_.getActive();
    std::string line = what + " " + why + "; " + std::string(neededFor) + " needs it";
    if (profile) line += " on " + profile->name;
    line += ", and the game cannot go on without it";
    if (stop_) stop_(line);
    LOG_FATAL(line);
    std::abort();
}

} // namespace wowee::game

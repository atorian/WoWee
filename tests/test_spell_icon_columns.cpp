// SpellIcon.dbc read through the loader every caller uses, over the install's
// own data.
//
// A DBCFile asked for the wrong column does not fail: it answers zero or an
// empty string for every row, forever, and icons simply do not appear. So the
// test is what the loader hands back from the real file - not which column
// numbers it uses, which a wrong pair would still satisfy in a test that only
// restated them.
#include <catch_amalgamated.hpp>

#include "test_support.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/spell_icon_paths.hpp"
#include "test_game_tables.hpp"

#include <fstream>
#include <sstream>

#include <cctype>
#include <string>
#include <unordered_map>

namespace {

bool isIconPath(const std::string& path) {
    std::string lower;
    for (char c : path) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.rfind("interface\\icons\\", 0) == 0 || lower.rfind("interface/icons/", 0) == 0;
}

}  // namespace

TEST_CASE("the loader reads every icon id to its icon path", "[spell-icon]") {
    // An install's extracted data sits beside its expansion profile, where
    // asset_extract puts it.
    wowee::pipeline::AssetManager assets;
    if (!assets.initialize(wowee::test::sourceRoot() + "Data/expansions/wotlk")) {
        WARN("no extracted WotLK data under Data/expansions/wotlk, skipping");
        return;
    }

    // Through WotLK's own layout, as the client reads it.
    std::ifstream layoutFile(wowee::test::sourceRoot() + "Data/expansions/wotlk/dbc_layouts.json");
    std::stringstream layoutText;
    layoutText << layoutFile.rdbuf();
    const wowee::test::GivenLayouts layouts(layoutText.str());
    wowee::test::GivenTables tables;
    tables.giveFile("SpellIcon.dbc", assets.readFile("DBFilesClient\\SpellIcon.dbc"));

    std::unordered_map<uint32_t, std::string> paths;
    wowee::pipeline::loadSpellIconPaths(tables, paths);
    REQUIRE(paths.size() > 100);

    size_t icons = 0;
    for (const auto& [id, path] : paths) {
        if (isIconPath(path)) ++icons;
    }
    INFO(icons << " of " << paths.size() << " paths are under Interface\\Icons");
    // Not all: a row or two of the shipped table is empty, and the loader
    // skips those rather than mapping them.
    CHECK(icons > paths.size() * 9 / 10);
    // An id read from the path column would be a string offset, and zero for
    // the rows that start the string block; there is no icon zero.
    CHECK(paths.count(0) == 0);
}

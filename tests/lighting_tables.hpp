#pragma once

// The Light*.dbc tables a LightingManager reads, under WotLK's column layouts:
// the files a test gives, and those it does not give as tables with no rows -
// an expansion has all five, and a test about volumes has nothing to say about
// skies.

#include "test_game_tables.hpp"

#include <map>
#include <string>
#include <vector>

namespace wowee::test {

/// The Light* entries of Data/expansions/wotlk/dbc_layouts.json.
inline constexpr const char* kLightLayouts = R"({
  "Light": {"ID": 0, "InnerRadius": 5, "LightParamsID": 7, "LightParamsIDClearUnderwater": 8,
            "LightParamsIDStorm": 9, "LightParamsIDStormUnderwater": 10, "MapID": 1,
            "OuterRadius": 6, "X": 2, "Y": 4, "Z": 3},
  "LightFloatBand": {"BlockIndex": 0, "NumKeyframes": 1, "TimeKey0": 2, "Value0": 18},
  "LightIntBand": {"BlockIndex": 0, "NumKeyframes": 1, "TimeKey0": 2, "Value0": 18},
  "LightParams": {"LightParamsID": 0, "LightSkyboxID": 2},
  "LightSkybox": {"Flags": 2, "ID": 0, "Path": 1}
})";

struct LightingTables {
    /// `files` by table name ("Light.dbc"), each the bytes of a WDBC file.
    explicit LightingTables(const std::map<std::string, std::vector<uint8_t>>& files) {
        const std::map<std::string, uint32_t> widths = {
            {"Light.dbc", 11}, {"LightParams.dbc", 3}, {"LightSkybox.dbc", 3},
            {"LightIntBand.dbc", 34}, {"LightFloatBand.dbc", 34}};
        for (const auto& [dbc, fields] : widths) {
            const auto given = files.find(dbc);
            if (given != files.end()) tables.giveFile(dbc, given->second);
            else tables.give(dbc, fields, {});
        }
    }

    GivenLayouts layouts{kLightLayouts};
    GivenTables tables;
};

} // namespace wowee::test

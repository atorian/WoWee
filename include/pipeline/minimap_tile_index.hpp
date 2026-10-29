#pragma once
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>
namespace wowee::pipeline {
/// Textures\Minimap\md5translate.trs: the name a minimap tile is asked for by
/// ("Northrend\map30_21") and the hashed file that holds it. Names are matched
/// without regard to case, as the client's archives are.
class MinimapTileIndex {
public:
    static MinimapTileIndex parse(std::span<const uint8_t> trs);
    /// The hashed file of a map's terrain tile, without ".blp", or null when
    /// the index has none.
    [[nodiscard]] const std::string* terrainTile(std::string_view map, int x, int y) const;
    /// The name terrainTile looks up, for messages about it.
    [[nodiscard]] static std::string terrainTileName(std::string_view map, int x, int y);
    [[nodiscard]] bool empty() const { return files_.empty(); }
private:
    std::unordered_map<std::string, std::string> files_;
};
}

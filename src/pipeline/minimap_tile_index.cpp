#include "pipeline/minimap_tile_index.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
namespace wowee::pipeline {
namespace {
std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}
std::string_view withoutBlp(std::string_view name) {
    if (name.size() >= 4 && lowered(name.substr(name.size() - 4)) == ".blp") name.remove_suffix(4);
    return name;
}
}
MinimapTileIndex MinimapTileIndex::parse(std::span<const uint8_t> trs) {
    MinimapTileIndex index;
    std::string_view text(reinterpret_cast<const char*>(trs.data()), trs.size());
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        index.files_.emplace(lowered(withoutBlp(line.substr(0, tab))),
                             std::string(withoutBlp(line.substr(tab + 1))));
    }
    return index;
}
// The client asks with "%s\map%d_%02d.blp" (wow.exe 3.3.5a, 0x979e10): the
// row is always two digits, so row 6 is "map42_06" and never "map42_6".
std::string MinimapTileIndex::terrainTileName(std::string_view map, int x, int y) {
    char tile[32];
    std::snprintf(tile, sizeof(tile), "\\map%d_%02d", x, y);
    return std::string(map) + tile;
}
// "%s_%03d_%02d_%02d.blp" (wow.exe 3.3.5a, 0x979df8).
std::string MinimapTileIndex::buildingTileName(std::string_view stem, int group, int x, int y) {
    char tile[32];
    std::snprintf(tile, sizeof(tile), "_%03d_%02d_%02d", group, x, y);
    return std::string(stem) + tile;
}
const std::string* MinimapTileIndex::buildingTile(std::string_view stem, int group, int x, int y) const {
    const auto found = files_.find(lowered(buildingTileName(stem, group, x, y)));
    return found == files_.end() ? nullptr : &found->second;
}
const std::string* MinimapTileIndex::terrainTile(std::string_view map, int x, int y) const {
    const auto found = files_.find(lowered(terrainTileName(map, x, y)));
    return found == files_.end() ? nullptr : &found->second;
}
}

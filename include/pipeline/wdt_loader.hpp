#pragma once

#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

struct WDTInfo {
    uint32_t mphdFlags = 0;
    [[nodiscard]] bool isWMOOnly() const { return mphdFlags & 0x01; }  // WDTF_GLOBAL_WMO

    std::string rootWMOPath;       // from MWMO chunk (null-terminated string)

    /// MAIN: which of the 64x64 tiles have an ADT (flag bit 1), at
    /// [tileY * 64 + tileX] - the tile x being MAIN's column.
    std::bitset<64 * 64> tilesWithTerrain;

    // MODF placement (only valid for WMO-only maps):
    float position[3] = {};        // ADT placement space coords
    float rotation[3] = {};        // degrees
    uint16_t flags = 0;
    uint16_t doodadSet = 0;
    /// The NameSetID WMOAreaTable.dbc rows for this placement carry.
    uint16_t nameSet = 0;
};

WDTInfo parseWDT(const std::vector<uint8_t>& data);

} // namespace pipeline
} // namespace wowee

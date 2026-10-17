#include "pipeline/liquid_types.hpp"

#include "core/logger.hpp"

namespace wowee {
namespace pipeline {

namespace {

// LiquidType.dbc rows the client turns a WMO group's old-style liquid into.
constexpr uint16_t kWmoWater = 13;
constexpr uint16_t kWmoOcean = 14;
constexpr uint16_t kWmoWaterInterior = 17;
constexpr uint16_t kWmoMagma = 19;
constexpr uint16_t kWmoSlime = 20;
// groupLiquid values the rule tests against.
constexpr uint32_t kGreenLava = 15;
constexpr uint32_t kEndBasicLiquids = 20;
constexpr uint32_t kFirstNonBasicLiquid = 21;
// MOGP flags: the group's water is the sea; the group is indoors.
constexpr uint32_t kGroupWaterIsOcean = 0x80000;
constexpr uint32_t kGroupInterior = 0x2000;

uint16_t toWmoLiquid(uint32_t basic, uint32_t groupFlags) {
    switch (basic & 3u) {
        case 0: return (groupFlags & kGroupWaterIsOcean) ? kWmoOcean : kWmoWater;
        case 1: return kWmoOcean;
        case 2: return kWmoMagma;
        default: return kWmoSlime;
    }
}

} // namespace

uint16_t basicLiquidTypeId(LiquidBasic basic) {
    return static_cast<uint16_t>(static_cast<uint8_t>(basic) + 1);
}

namespace {

uint16_t groupLiquidRow(uint32_t groupLiquid, uint32_t groupFlags, bool rootUsesDbcIds,
                        uint8_t tileLiquid) {
    if (rootUsesDbcIds) {
        // A group that names no liquid but has a liquid chunk all the same:
        // its tiles say what it is.
        if (groupLiquid == 0) return toWmoLiquid(tileLiquid, groupFlags);
        if (groupLiquid < kFirstNonBasicLiquid) return toWmoLiquid(groupLiquid - 1, groupFlags);
        return static_cast<uint16_t>(groupLiquid);
    }
    if (groupLiquid == kGreenLava) return toWmoLiquid(tileLiquid, groupFlags);
    if (groupLiquid < kEndBasicLiquids) return toWmoLiquid(groupLiquid, groupFlags);
    return static_cast<uint16_t>(groupLiquid + 1);
}

} // namespace

uint16_t wmoGroupLiquidType(uint32_t groupLiquid, uint32_t groupFlags, bool rootUsesDbcIds,
                            uint8_t tileLiquid) {
    const uint16_t row = groupLiquidRow(groupLiquid, groupFlags, rootUsesDbcIds, tileLiquid);
    // Water indoors is its own row, as the client draws it.
    if (row == kWmoWater && (groupFlags & kGroupInterior)) return kWmoWaterInterior;
    return row;
}

LiquidTypeTable LiquidTypeTable::basicOnly() {
    LiquidTypeTable table;
    const struct { LiquidBasic basic; const char* name; } rows[] = {
        {LiquidBasic::Water, "Water"}, {LiquidBasic::Ocean, "Ocean"},
        {LiquidBasic::Magma, "Magma"}, {LiquidBasic::Slime, "Slime"},
    };
    for (const auto& row : rows) {
        LiquidTypeInfo info;
        info.id = basicLiquidTypeId(row.basic);
        info.name = row.name;
        info.basic = row.basic;
        table.add(std::move(info));
    }
    return table;
}

const LiquidTypeInfo* LiquidTypeTable::find(uint32_t id) const {
    const auto it = rows_.find(id);
    return it != rows_.end() ? &it->second : nullptr;
}

LiquidBasic LiquidTypeTable::basicOf(uint32_t id) const {
    if (const LiquidTypeInfo* info = find(id)) return info->basic;
    if (unknownSaid_.insert(id).second) {
        LOG_WARNING("Liquid types: liquid id ", id, " is not in LiquidType.dbc - drawn and "
                    "swum in as water");
    }
    return LiquidBasic::Water;
}

void LiquidTypeTable::add(LiquidTypeInfo info) {
    const uint32_t id = info.id;
    rows_[id] = std::move(info);
}

} // namespace pipeline
} // namespace wowee

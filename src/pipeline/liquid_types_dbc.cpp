// LiquidTypeTable read from LiquidType.dbc. Apart from the rest of the table
// so that what only needs the ids - the ADT and WMO loaders - does not need
// the DBC reader to link.
#include "pipeline/liquid_types.hpp"

#include "core/logger.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

namespace wowee {
namespace pipeline {

LiquidTypeTable LiquidTypeTable::fromDbc(const DBCFile& dbc, const DBCFieldMap* layout,
                                         const std::string& source) {
    LiquidTypeTable table;
    if (!layout) {
        LOG_WARNING("Liquid types: no column layout for LiquidType in ", source,
                    " - only the four basic liquids are known, drawn without their textures");
        return basicOnly();
    }
    const uint32_t idCol = layout->field("ID");
    const uint32_t nameCol = layout->field("Name");
    const uint32_t typeCol = layout->field("Type");
    const uint32_t materialCol = layout->field("MaterialID");
    const uint32_t textureCol = layout->field("Texture");
    constexpr uint32_t kMissing = 0xFFFFFFFFu;
    if (idCol == kMissing || typeCol == kMissing) {
        LOG_WARNING("Liquid types: the LiquidType layout in ", source,
                    " names no ID or Type column - only the four basic liquids are known");
        return basicOnly();
    }

    for (uint32_t r = 0; r < dbc.getRecordCount(); ++r) {
        LiquidTypeInfo info;
        info.id = dbc.getUInt32(r, idCol);
        const uint32_t type = dbc.getUInt32(r, typeCol);
        if (type > 3) {
            LOG_WARNING("Liquid types: LiquidType ", info.id, " in ", source, " has type ", type,
                        ", not one of the four basic kinds - left out, and liquid of that id is "
                        "taken for water");
            continue;
        }
        info.basic = static_cast<LiquidBasic>(type);
        if (nameCol != kMissing) info.name = dbc.getString(r, nameCol);
        if (materialCol != kMissing) info.materialId = dbc.getUInt32(r, materialCol);
        if (textureCol != kMissing) info.texturePattern = dbc.getString(r, textureCol);
        table.add(std::move(info));
    }
    return table;
}

} // namespace pipeline
} // namespace wowee

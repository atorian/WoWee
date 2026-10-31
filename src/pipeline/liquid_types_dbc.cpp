// LiquidTypeTable read from LiquidType.dbc. Apart from the rest of the table
// so that what only needs the ids - the ADT and WMO loaders - does not need
// the DBC reader to link.
#include "pipeline/liquid_types.hpp"

#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"

namespace wowee {
namespace pipeline {

LiquidTypeTable LiquidTypeTable::fromDbc(const RequiredTable& liquidTypes) {
    LiquidTypeTable table;
    const DBCFile& dbc = *liquidTypes;
    const uint32_t idCol = liquidTypes.column("ID");
    const uint32_t nameCol = liquidTypes.column("Name");
    const uint32_t typeCol = liquidTypes.column("Type");
    const uint32_t materialCol = liquidTypes.column("MaterialID");
    const uint32_t textureCol = liquidTypes.column("Texture");
    const std::string source = "LiquidType.dbc";

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
        info.name = dbc.getString(r, nameCol);
        info.materialId = dbc.getUInt32(r, materialCol);
        info.texturePattern = dbc.getString(r, textureCol);
        table.add(std::move(info));
    }
    return table;
}

} // namespace pipeline
} // namespace wowee

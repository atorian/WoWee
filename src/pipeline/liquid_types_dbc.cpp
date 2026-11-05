// LiquidTypeTable read from LiquidType.dbc. Apart from the rest of the table
// so that what only needs the ids - the ADT and WMO loaders - does not need
// the DBC reader to link.
#include "pipeline/liquid_types.hpp"

#include "foundation/logger.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"

namespace wowee {
namespace pipeline {

LiquidTypeTable LiquidTypeTable::fromDbc(const RequiredTable& liquidTypes, const RequiredTable& liquidMaterials) {
    LiquidTypeTable table;
    const DBCFile& dbc = *liquidTypes;
    const uint32_t idCol = liquidTypes.column("ID");
    const uint32_t nameCol = liquidTypes.column("Name");
    const uint32_t typeCol = liquidTypes.column("Type");
    const uint32_t materialCol = liquidTypes.column("MaterialID");
    const uint32_t textureCol = liquidTypes.column("Texture");
    const uint32_t flagsCol = liquidTypes.column("Flags");
    const std::string source = "LiquidType.dbc";

    std::unordered_map<uint32_t, uint16_t> vertexFormats;
    {
        const DBCFile& materials = *liquidMaterials;
        const uint32_t materialIdCol = liquidMaterials.column("ID");
        const uint32_t lvfCol = liquidMaterials.column("LVF");
        for (uint32_t r = 0; r < materials.getRecordCount(); ++r) {
            vertexFormats[materials.getUInt32(r, materialIdCol)] =
                static_cast<uint16_t>(materials.getUInt32(r, lvfCol));
        }
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
        info.name = dbc.getString(r, nameCol);
        info.materialId = dbc.getUInt32(r, materialCol);
        info.flags = dbc.getUInt32(r, flagsCol);
        if (const auto format = vertexFormats.find(info.materialId); format != vertexFormats.end()) {
            info.vertexFormat = format->second;
        } else {
            LOG_WARNING("Liquid types: LiquidType ", info.id, " in ", source, " names material ",
                        info.materialId, ", which LiquidMaterial.dbc does not have - a legacy "
                        "liquid layer of this type cannot be collided with");
            info.materialKnown = false;
        }
        info.texturePattern = dbc.getString(r, textureCol);
        table.add(std::move(info));
    }
    return table;
}

} // namespace pipeline
} // namespace wowee

#include "pipeline/spell_icon_paths.hpp"

#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"

namespace wowee::pipeline {

void loadSpellIconPaths(const GameTables& tables,
                        std::unordered_map<uint32_t, std::string>& out) {
    const RequiredTable dbc(tables, "SpellIcon.dbc", "spell and talent icons");
    const uint32_t idColumn = dbc.column("ID");
    const uint32_t pathColumn = dbc.column("Path");

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const uint32_t id = dbc->getUInt32(i, idColumn);
        std::string path = dbc->getString(i, pathColumn);
        // A row with no path is a hole in the table rather than an icon.
        if (id > 0 && !path.empty()) out[id] = std::move(path);
    }
}

}  // namespace wowee::pipeline

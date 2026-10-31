#include "pipeline/required_table.hpp"

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <utility>

namespace wowee::pipeline {

RequiredTable::RequiredTable(const GameTables& tables, std::string dbc, std::string_view neededFor,
                             uint32_t fields)
    : tables_(tables), dbc_(std::move(dbc)), neededFor_(neededFor),
      table_(tables.table(dbc_, fields, neededFor)) {}

const DBCFieldMap& RequiredTable::layout() const {
    const std::string name = dbc_.substr(0, dbc_.rfind('.'));
    const DBCLayout* layouts = getActiveDBCLayout();
    const DBCFieldMap* map = layouts ? layouts->getLayout(name) : nullptr;
    if (!map) tables_.stop(dbc_, neededFor_, "has no column layout in dbc_layouts.json");
    return *map;
}

uint32_t RequiredTable::column(const std::string& field) const { return column(field, 1); }

uint32_t RequiredTable::column(const std::string& field, uint32_t count) const {
    const uint32_t index = layout().tryField(field);
    if (index == kAbsent) {
        tables_.stop(dbc_, neededFor_, "has no column '" + field + "' in dbc_layouts.json");
    }
    const uint32_t last = index + count - 1;
    if (last >= table_->getFieldCount()) {
        tables_.stop(dbc_, neededFor_,
                     "is " + std::to_string(table_->getFieldCount()) + " columns wide, and '" + field +
                         "' at column " + std::to_string(index) + " needs " + std::to_string(last + 1));
    }
    return index;
}

uint32_t RequiredTable::optionalColumn(const std::string& field) const {
    return layout().tryField(field) == kAbsent ? kAbsent : column(field);
}

} // namespace wowee::pipeline

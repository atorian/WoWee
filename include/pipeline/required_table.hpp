#pragma once

#include "pipeline/game_tables.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace wowee::pipeline {

struct DBCFieldMap;

/// A table the running expansion uses, with its columns as that expansion's
/// dbc_layouts.json names them.
///
/// Reading a column the layout does not name used to fall back to a column
/// number written into the reader - the WotLK one, usually - which on any
/// other shape of the file reads a neighbouring column and draws or says
/// something wrong without a word. Here a column the layout does not name, or
/// one past the end of the file, stops the game like a table that cannot be
/// read (AGENTS.md, "Game data: what an expansion uses is required").
class RequiredTable {
public:
    /// `dbc` ("Light.dbc"), at least `fields` columns wide for the columns
    /// read at fixed places, which stops the game when it cannot be read.
    /// `neededFor` names what asked, for that line.
    RequiredTable(const GameTables& tables, std::string dbc, std::string_view neededFor,
                  uint32_t fields = 1);

    [[nodiscard]] const DBCFile& operator*() const { return *table_; }
    [[nodiscard]] const DBCFile* operator->() const { return table_.get(); }
    [[nodiscard]] const DBCFile* get() const { return table_.get(); }
    /// The table, to keep past this object.
    [[nodiscard]] const GameTables::Table& shared() const { return table_; }

    /// The column the layout names `field`.
    [[nodiscard]] uint32_t column(const std::string& field) const;
    /// The first of `count` columns in a row from the one the layout names
    /// `field` (TimeKey0 .. TimeKey15), all of which the file must have.
    [[nodiscard]] uint32_t column(const std::string& field, uint32_t count) const;
    /// What optionalColumn() answers for a column the layout does not name.
    static constexpr uint32_t kAbsent = 0xFFFFFFFFu;
    /// A column only some expansions' file has, which the running one's layout
    /// names or not: kAbsent when it does not. One it names must be there,
    /// like any other.
    [[nodiscard]] uint32_t optionalColumn(const std::string& field) const;

    /// The whole layout, for the readers that tell the file's shape from its
    /// width (detectCharSectionsFields and the like). Stops when there is none.
    [[nodiscard]] const DBCFieldMap& layout() const;

private:
    const GameTables& tables_;
    std::string dbc_;
    std::string neededFor_;
    GameTables::Table table_;
};

} // namespace wowee::pipeline

#pragma once

// The expansion's tables for a test of a layer below the game - the renderer,
// the sound, the interface's readers - which takes pipeline::GameTables rather
// than the game's GameData: the tables the test gives, under the column
// layout it gives, and the stop the client logs and aborts on thrown instead.

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/game_tables.hpp"
#include "test_support.hpp"

#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace wowee::test {

/// The client stopped: what it logs as LOG_FATAL before it aborts.
struct TablesStopped : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The running expansion's list is the tables given; a table not given is one
/// the expansion does not use.
class GivenTables final : public pipeline::GameTables {
public:
    /// One cell: a number, or a string put in the string block.
    struct Cell {
        Cell(uint32_t v) : number(v) {}
        Cell(int v) : number(static_cast<uint32_t>(v)) {}
        Cell(const char* s) : text(s), isText(true) {}
        uint32_t number = 0;
        std::string text;
        bool isText = false;
    };
    /// A row by column; the columns it does not name are zero.
    using Row = std::map<uint32_t, Cell>;

    /// `dbc`, `fields` columns wide, these rows in order.
    void give(const std::string& dbc, uint32_t fields, const std::vector<Row>& rows) {
        std::string strings(1, '\0');
        std::vector<std::vector<uint32_t>> cells;
        for (const Row& row : rows) {
            std::vector<uint32_t> out(fields, 0);
            for (const auto& [column, cell] : row) {
                if (cell.isText) {
                    out.at(column) = static_cast<uint32_t>(strings.size());
                    strings += cell.text;
                    strings += '\0';
                } else {
                    out.at(column) = cell.number;
                }
            }
            cells.push_back(std::move(out));
        }
        auto table = std::make_shared<pipeline::DBCFile>();
        if (!table->load(wdbcFile(fields, cells, strings))) throw std::logic_error(dbc + " does not load");
        tables_[dbc] = std::move(table);
    }
    /// `dbc` as these bytes of a WDBC file.
    void giveFile(const std::string& dbc, const std::vector<uint8_t>& bytes) {
        auto table = std::make_shared<pipeline::DBCFile>();
        if (!table->load(bytes)) throw std::logic_error(dbc + " does not load");
        tables_[dbc] = std::move(table);
    }
    /// `dbc` is on the list and cannot be read: a broken install.
    void giveUnreadable(const std::string& dbc) { tables_[dbc] = std::make_shared<pipeline::DBCFile>(); }

    [[nodiscard]] bool opened(const std::string& dbc) const { return opened_.count(dbc) != 0; }

    [[nodiscard]] bool has(const std::string& dbc) const override { return tables_.count(dbc) != 0; }
    [[nodiscard]] Table table(const std::string& dbc, uint32_t fields,
                              std::string_view neededFor) const override {
        const auto it = tables_.find(dbc);
        if (it == tables_.end()) stop(dbc, neededFor, "is not on the expansion's list of tables it uses");
        opened_.insert(dbc);
        if (!it->second->isLoaded()) stop(dbc, neededFor, "could not be read");
        if (it->second->getFieldCount() < fields) stop(dbc, neededFor, "is narrower than what is read");
        return it->second;
    }
    [[noreturn]] void stop(const std::string& what, std::string_view neededFor,
                           const std::string& why) const override {
        throw TablesStopped(what + " " + why + "; " + std::string(neededFor) + " needs it");
    }

private:
    std::map<std::string, Table> tables_;
    mutable std::set<std::string> opened_;
};

/// The active column layouts, from the text of a dbc_layouts.json, for as long
/// as this lives.
class GivenLayouts {
public:
    explicit GivenLayouts(const std::string& json) : previous_(pipeline::getActiveDBCLayout()) {
        if (!layouts_.loadFromJsonText(json, "the test's layouts")) throw std::logic_error("layouts do not parse");
        pipeline::setActiveDBCLayout(&layouts_);
    }
    ~GivenLayouts() { pipeline::setActiveDBCLayout(previous_); }
    GivenLayouts(const GivenLayouts&) = delete;
    GivenLayouts& operator=(const GivenLayouts&) = delete;

private:
    pipeline::DBCLayout layouts_;
    const pipeline::DBCLayout* previous_;
};

} // namespace wowee::test

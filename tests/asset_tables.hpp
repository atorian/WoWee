#pragma once

// The expansion's tables for a test that runs the renderer on the real data:
// read through the asset manager as the client reads them, under the
// expansion's column layouts. The renderer stops without them; here a table
// that cannot be read fails the test instead.

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/game_tables.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace wowee::test {

class AssetTables final : public pipeline::GameTables {
public:
    /// The tables under `data` (the Data directory) for WotLK, its column
    /// layouts made the active ones, as the client sets them before the
    /// renderer reads its tables through them.
    AssetTables(pipeline::AssetManager& assets, const std::string& data) : assets_(assets) {
        if (!layouts_.loadFromJson(data + "/expansions/wotlk/dbc_layouts.json"))
            throw std::runtime_error("WotLK DBC layouts unavailable under " + data);
        pipeline::setActiveDBCLayout(&layouts_);
    }
    ~AssetTables() override { pipeline::setActiveDBCLayout(nullptr); }
    AssetTables(const AssetTables&) = delete;
    AssetTables& operator=(const AssetTables&) = delete;

    [[nodiscard]] bool has(const std::string& dbc) const override {
        const Table t = assets_.loadDBC(dbc);
        return t && t->isLoaded();
    }
    [[nodiscard]] Table table(const std::string& dbc, uint32_t, std::string_view neededFor) const override {
        Table t = assets_.loadDBC(dbc);
        if (!t || !t->isLoaded()) stop(dbc, neededFor, "could not be read");
        return t;
    }
    [[noreturn]] void stop(const std::string& what, std::string_view neededFor,
                           const std::string& why) const override {
        throw std::runtime_error(what + " " + why + " (" + std::string(neededFor) + ")");
    }

private:
    pipeline::AssetManager& assets_;
    pipeline::DBCLayout layouts_;
};

}  // namespace wowee::test

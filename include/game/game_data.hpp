#pragma once

#include "pipeline/game_tables.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::game {

class ExpansionRegistry;
struct ExpansionProfile;

/// The game data the running expansion uses - the client's tables (*.dbc)
/// the game reads its rules and names from, and the expansion's own files in
/// Data/expansions/<expansion>/ - as that expansion's profile lists them
/// (expansion.json, "tables" and "files"). AGENTS.md, "Game data: what an
/// expansion uses is required":
///
///   * what is on the list and cannot be read, or is narrower than what is
///     asked of it, stops the game;
///   * what is not on the list is a feature the expansion does not have, and
///     is never opened: has() says so, and asking for it anyway stops the game
///     as the bug it is;
///   * checkAll() reads the whole list when the expansion's data is opened,
///     so a broken install stops at start.
///
/// Game data, like the emote and stat tables: the client owns one and hands it
/// over (GameServices::gameData), and to the layers below the game as the
/// pipeline::GameTables it is. A test hands over the tables it gives.
class GameData final : public pipeline::GameTables {
public:
    /// A table by its file name; nullptr, or a table that did not load, when
    /// it cannot be read.
    using Read = std::function<Table(const std::string& dbc)>;
    /// How the game stops, given the line that says why. Unset, it is
    /// LOG_FATAL and std::abort(); a test throws instead, to see it happen.
    using Stop = std::function<void(const std::string& line)>;

    GameData(Read read, const ExpansionRegistry& expansions, Stop stop = {});
    /// Read from the client's data. The asset manager is asked at the first
    /// read, when it has been initialized or has failed to be.
    static Read fromAssets(pipeline::AssetManager* assets);

    /// The running expansion. Stops the game when none is.
    [[nodiscard]] const ExpansionProfile& expansion() const;
    /// Whether the running expansion uses `dbc`: what the code that offers a
    /// feature asks before offering it.
    [[nodiscard]] bool has(const std::string& dbc) const override;

    /// `dbc`, at least `fields` columns wide. `neededFor` names what asked, for
    /// the line that stops the game when the table cannot answer it.
    [[nodiscard]] Table table(const std::string& dbc, uint32_t fields,
                              std::string_view neededFor) const override;
    /// The path of one of the running expansion's own files, which must be on
    /// its list and be there.
    [[nodiscard]] std::string file(const std::string& name, std::string_view neededFor) const;

    /// Every file and table on the running expansion's list, read now.
    void checkAll() const;

    /// Stops the game: `what` cannot give `neededFor` what it asked, for `why`.
    [[noreturn]] void stop(const std::string& what, std::string_view neededFor,
                           const std::string& why) const override;

private:
    Read read_;
    const ExpansionRegistry& expansions_;
    Stop stop_;
};

} // namespace wowee::game

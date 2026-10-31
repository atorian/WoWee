#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace wowee::pipeline { class AssetManager; class DBCFile; }

namespace wowee::game {

/// The client's tables (*.dbc) the game reads its rules and names from: the
/// languages, the forms' action bars, the spells, the taxi network, the
/// emotes. The game has nothing true to answer without them, so a table that
/// cannot be read, or is narrower than what is asked of it, stops the game
/// (AGENTS.md, "Errors are logged, never swallowed").
///
/// Game data, like the emote and stat tables: the client owns one and hands it
/// over (GameServices::gameData). A test hands over the tables it gives.
class GameData {
public:
    using Table = std::shared_ptr<const pipeline::DBCFile>;
    /// A table by its file name; nullptr, or a table that did not load, when
    /// it cannot be read.
    using Read = std::function<Table(const std::string& dbc)>;

    explicit GameData(Read read);
    /// Read from the client's data. The asset manager is asked at the first
    /// read, when it has long been initialized.
    static Read fromAssets(pipeline::AssetManager* assets);

    /// `dbc`, at least `fields` columns wide. `neededFor` names what asked, for
    /// the line that stops the game when the table cannot answer it.
    [[nodiscard]] Table table(const std::string& dbc, uint32_t fields,
                              std::string_view neededFor) const;

    /// Stops the game: `dbc` cannot give `neededFor` what it asked, for `why`.
    [[noreturn]] static void stop(const std::string& dbc, std::string_view neededFor,
                                  const std::string& why);

private:
    Read read_;
};

} // namespace wowee::game

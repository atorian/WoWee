#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace wowee::pipeline {

class DBCFile;

/// The client tables (*.dbc) the running expansion uses, for the layers below
/// the game - the renderer, the sound, the asset pipeline - which may not
/// include it. game::GameData implements it; the client hands that over.
///
/// AGENTS.md, "Game data: what an expansion uses is required": a table on the
/// running expansion's list that cannot be read, or is narrower than what is
/// asked of it, stops the game; a table not on it is a feature the expansion
/// does not have, which the code offering it asks about with has() and never
/// opens.
class GameTables {
public:
    using Table = std::shared_ptr<const DBCFile>;

    virtual ~GameTables() = default;

    /// Whether the running expansion uses `dbc` ("Spell.dbc").
    [[nodiscard]] virtual bool has(const std::string& dbc) const = 0;

    /// `dbc`, at least `fields` columns wide. `neededFor` names what asked, for
    /// the line that stops the game when the table cannot answer it.
    [[nodiscard]] virtual Table table(const std::string& dbc, uint32_t fields,
                                      std::string_view neededFor) const = 0;

    /// Stops the game: `what` cannot give `neededFor` what it asked, for `why`.
    [[noreturn]] virtual void stop(const std::string& what, std::string_view neededFor,
                                   const std::string& why) const = 0;
};

} // namespace wowee::pipeline

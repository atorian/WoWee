#pragma once

#include "game/character.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::core {

/// Which character a CharacterSession belongs to.
struct CharacterIdentity {
    uint64_t guid = 0;
    /// Keys the per-character saved variables. Empty when it is not known,
    /// which skips them rather than guessing at a file.
    std::string name;
};

/// The character the game has just logged in, from what the game knows.
///
/// The character list is the authority for the name: it is what the player
/// picked from, and it is current for every character on the account. The
/// name cache (`cachedName`, GameHandler::lookupName of the guid) is asked
/// only when the list does not have the guid - it is filled by queries and can
/// lag a login.
inline CharacterIdentity characterIdentityOf(uint64_t playerGuid,
                                             const std::vector<game::Character>& characters,
                                             const std::string& cachedName) {
    CharacterIdentity identity{playerGuid, {}};
    for (const auto& c : characters) {
        if (c.guid == playerGuid) { identity.name = c.name; break; }
    }
    if (identity.name.empty()) identity.name = cachedName;
    return identity;
}

} // namespace wowee::core

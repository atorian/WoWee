#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "reputation/domain/events.hpp"
#include "reputation/domain/records.hpp"

namespace wowee::reputation {

class FactionCatalog;

/// A faction the player has a standing with, as the reputation panel lists it.
struct ReputationEntry {
    uint32_t factionId = 0;
    uint32_t reputationIndex = 0;
    std::string name;
    uint8_t flags = 0;
};

/// One line of the reputation panel as it is drawn: the flat list grouped
/// under the headers Faction.dbc's parent chain describes.
///
/// The server sends standings and nothing else - no categories, no order
/// beyond its own list. Both the headers and the nesting come from
/// ParentFactionID, and a header is simply a faction that some visible
/// faction descends from. A header may have a standing of its own (Alliance
/// does), which is why hasRep is a separate answer from isHeader.
struct ReputationRow {
    uint32_t factionId = 0;
    /// The server's index, and 0 for a header the player has no standing
    /// with - such a row is drawn but cannot be acted on.
    uint32_t reputationIndex = 0;
    std::string name;
    uint8_t flags = 0;
    bool isHeader = false;    ///< something below it descends from this
    bool isChild = false;     ///< drawn indented, under a header
    bool hasRep = false;      ///< the player has a standing to show
};

/// The player's standings, the faction being watched, and which groups of the
/// reputation panel are closed.
class Reputation {
public:
    /// SMSG_INITIALIZE_FACTIONS, each standing already made whole.
    std::vector<ReputationChange> initialize(std::vector<FactionStanding> standings, const FactionCatalog& catalog);
    /// SMSG_SET_FACTION_STANDING for one faction, made whole. A standing that
    /// moved is watched from then on.
    std::vector<ReputationChange> changeStanding(uint32_t reputationIndex, uint32_t factionId, int32_t standing,
                                                 const FactionCatalog& catalog);
    /// SMSG_SET_FACTION_ATWAR.
    std::vector<ReputationChange> setAtWar(uint32_t reputationIndex, bool atWar);
    /// SMSG_SET_FACTION_VISIBLE.
    void setVisible(uint32_t reputationIndex, bool visible);
    /// The player declares war or makes peace. False when there is no such
    /// standing, or the server forbids war with it.
    bool declareWar(uint32_t reputationIndex, bool atWar);
    /// The player moves a faction to or from the inactive list. False when
    /// there is no such standing.
    bool setInactive(uint32_t reputationIndex, bool inactive);
    void watch(uint32_t factionId) { watched_ = factionId; }
    /// Opens or closes a group; false when nothing changed.
    bool collapse(uint32_t factionId, bool collapsed);
    /// The closed groups as the character's settings saved them.
    void restoreCollapsed(std::unordered_set<uint32_t> factionIds);
    /// The reference data the list is resolved against was read.
    void catalogChanged() { dirty_ = true; }
    /// A character is logging in: none of the last one's standings, watched
    /// faction or closed groups is theirs. Its own closed groups come back
    /// with its settings.
    void beginCharacter() { *this = Reputation{}; }

    /// A faction's standing: the latest that arrived for it, or the one it
    /// logged in with. Zero when unknown.
    [[nodiscard]] int32_t standingOf(uint32_t factionId, const FactionCatalog& catalog) const;
    [[nodiscard]] const std::unordered_map<uint32_t, int32_t>& byFaction() const { return byFaction_; }
    /// Null when there is no standing at that index.
    [[nodiscard]] const FactionStanding* at(uint32_t reputationIndex) const;
    [[nodiscard]] uint32_t watched() const { return watched_; }
    [[nodiscard]] bool isCollapsed(uint32_t factionId) const { return collapsed_.count(factionId) > 0; }
    [[nodiscard]] const std::unordered_set<uint32_t>& collapsed() const { return collapsed_; }

    /// Visible factions in the server's own order. Empty until the standings arrive.
    [[nodiscard]] const std::vector<ReputationEntry>& list(const FactionCatalog& catalog) const;
    /// Every row the panel draws, depth first, without the members of a closed
    /// group. What GetNumFactions counts and GetFactionInfo indexes.
    [[nodiscard]] const std::vector<ReputationRow>& rows(const FactionCatalog& catalog) const;

private:
    void rebuild(const FactionCatalog& catalog) const;

    std::vector<FactionStanding> standings_;
    std::unordered_map<uint32_t, int32_t> byFaction_;
    uint32_t watched_ = 0;
    std::unordered_set<uint32_t> collapsed_;

    mutable bool dirty_ = true;
    mutable std::vector<ReputationEntry> list_;
    mutable std::vector<ReputationRow> rows_;
};

} // namespace wowee::reputation

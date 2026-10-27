#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::reputation {

struct FactionStanding;
struct ReputationEntry;
struct ReputationRow;

/// Reputation as the interface, the addon layer and the hostility rules use it.
/// Faction.dbc is read on the first ask that needs it.
class IReputation {
public:
    virtual ~IReputation() = default;

    /// Visible factions in the server's own order. Empty until the standings arrive.
    [[nodiscard]] virtual const std::vector<ReputationEntry>& getReputationList() const = 0;
    /// The rows the reputation panel draws. What GetNumFactions counts and
    /// GetFactionInfo indexes, so every binding taking a "faction index" must
    /// resolve it against these.
    [[nodiscard]] virtual const std::vector<ReputationRow>& getReputationRows() const = 0;
    /// The player's current standing with a faction.
    [[nodiscard]] virtual int32_t getFactionStanding(uint32_t factionId) const = 0;
    /// Faction id to standing, for every faction a standing arrived for.
    [[nodiscard]] virtual const std::unordered_map<uint32_t, int32_t>& getFactionStandings() const = 0;
    /// The standing at a reputation index, or null.
    [[nodiscard]] virtual const FactionStanding* getStandingAt(uint32_t reputationIndex) const = 0;
    /// Empty when unknown.
    [[nodiscard]] virtual const std::string& getFactionName(uint32_t factionId) const = 0;
    /// The faction at a reputation index, or zero.
    [[nodiscard]] virtual uint32_t getFactionIdByRepListId(uint32_t reputationIndex) const = 0;

    [[nodiscard]] virtual bool isFactionCollapsed(uint32_t factionId) const = 0;
    virtual void setFactionCollapsed(uint32_t factionId, bool collapsed) = 0;
    /// Client-side: the server has no opinion about it, so it is saved with
    /// the rest of the character's settings.
    [[nodiscard]] virtual const std::unordered_set<uint32_t>& getCollapsedFactions() const = 0;
    virtual void restoreCollapsedFactions(std::unordered_set<uint32_t> factionIds) = 0;

    [[nodiscard]] virtual uint32_t getWatchedFactionId() const = 0;
    virtual void setWatchedFactionId(uint32_t factionId) = 0;
    /// Declare war or make peace (CMSG_SET_FACTION_ATWAR). Nothing on a
    /// faction the server forbids war with. The flag changes at once; the
    /// server echoes SMSG_SET_FACTION_ATWAR to confirm.
    virtual void setFactionAtWar(uint32_t reputationIndex, bool atWar) = 0;
    /// Move a faction to or from the inactive list (CMSG_SET_FACTION_INACTIVE).
    virtual void setFactionInactive(uint32_t reputationIndex, bool inactive) = 0;

    /// Reads Faction.dbc now rather than on the first ask.
    virtual void loadFactions() = 0;
};

/// `reputation`, or that of a client with no game: no standings, no names, and
/// every request dropped - for a reader that would otherwise test for it at
/// every call.
[[nodiscard]] IReputation& orNoReputation(IReputation* reputation);

/// What the protocol layer tells reputation. Standings are as the server sends
/// them: what has been earned, without where each faction starts.
class IReputationFeed {
public:
    virtual ~IReputationFeed() = default;

    /// SMSG_INITIALIZE_FACTIONS: flags and earned standing by reputation index.
    virtual void onInitialize(std::vector<FactionStanding> earned) = 0;
    /// SMSG_SET_FACTION_STANDING, one entry.
    virtual void onStanding(uint32_t reputationIndex, int32_t earned) = 0;
    /// SMSG_SET_FACTION_ATWAR.
    virtual void onAtWar(uint32_t reputationIndex, bool atWar) = 0;
    /// SMSG_SET_FACTION_VISIBLE.
    virtual void onVisible(uint32_t reputationIndex, bool visible) = 0;
    /// A character is logging in: none of the last one's standings, watched
    /// faction or closed groups is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::reputation

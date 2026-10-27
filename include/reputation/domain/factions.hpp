#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::reputation {

/// No reputation list index: the faction carries no standing.
inline constexpr uint32_t kNoReputationIndex = 0xFFFFFFFFu;

/// One Faction.dbc row, as the reference data was read.
struct FactionRecord {
    uint32_t    id = 0;
    /// ReputationListID: the dense index the server sends standings by, or
    /// kNoReputationIndex for a faction the player cannot have a standing with.
    uint32_t    reputationIndex = kNoReputationIndex;
    /// ParentFactionID, or zero for a faction at the top of the tree.
    uint32_t    parentId = 0;
    std::string name;
    /// Where a standing starts, in four slots: race masks, class masks, then
    /// the base standing of each slot. Empty when the file does not carry them.
    bool hasBase = false;
    std::array<int32_t, 12> base{};
};

/// What Faction.dbc says: names, the tree the reputation panel groups by, which
/// faction each reputation index belongs to, and where each standing starts.
class FactionCatalog {
public:
    void describe(const std::vector<FactionRecord>& rows);

    /// Empty when unknown.
    [[nodiscard]] const std::string& name(uint32_t factionId) const;
    /// The faction this one is grouped under, or zero for a top-level one.
    [[nodiscard]] uint32_t parentOf(uint32_t factionId) const;
    /// The faction at a reputation index, or zero.
    [[nodiscard]] uint32_t factionAt(uint32_t reputationIndex) const;
    /// The reputation index of a faction, or kNoReputationIndex.
    [[nodiscard]] uint32_t reputationIndexOf(uint32_t factionId) const;
    /// Where this character's standing with a faction starts, by race and class.
    ///
    /// The part of every standing the server leaves out: SMSG_INITIALIZE_FACTIONS
    /// and SMSG_SET_FACTION_STANDING carry what has been earned, and the whole
    /// is that on top of this - the server's own total is the two together.
    /// Read as the whole, every faction with a starting value came out wrong:
    /// an Alliance character starts at -1200 with the Kurenai, so 900 earned
    /// showed as Neutral while the server held them at Unfriendly and Telaar
    /// would not speak to them.
    ///
    /// The server's own rule, slot by slot: a slot for this race, or a
    /// class-only slot, whose class mask is this class or empty.
    [[nodiscard]] int32_t baseStanding(uint32_t factionId, uint8_t race, uint8_t playerClass) const;
    [[nodiscard]] size_t size() const { return names_.size(); }

private:
    std::unordered_map<uint32_t, std::string> names_;
    std::unordered_map<uint32_t, uint32_t> parents_;
    std::unordered_map<uint32_t, uint32_t> byIndex_;
    std::unordered_map<uint32_t, uint32_t> indexOf_;
    std::unordered_map<uint32_t, std::array<int32_t, 12>> base_;
};

} // namespace wowee::reputation

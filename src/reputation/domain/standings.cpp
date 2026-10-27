#include "reputation/domain/standings.hpp"

#include "reputation/domain/factions.hpp"

#include <algorithm>
#include <ranges>

namespace wowee::reputation {

std::vector<ReputationChange> Reputation::initialize(std::vector<FactionStanding> standings,
                                                     const FactionCatalog& catalog) {
    standings_ = std::move(standings);
    // Also keyed by faction, which is how everything that draws a standing
    // asks for one: the packet is indexed by reputation index.
    byFaction_.clear();
    for (size_t i = 0; i < standings_.size(); ++i) {
        const uint32_t factionId = catalog.factionAt(static_cast<uint32_t>(i));
        if (factionId != 0) byFaction_[factionId] = standings_[i].standing;
    }
    dirty_ = true;
    return {StandingsInitialized{}};
}

std::vector<ReputationChange> Reputation::changeStanding(uint32_t reputationIndex, uint32_t factionId,
                                                         int32_t standing, const FactionCatalog& catalog) {
    // The standing before is the one the character logged in with, until the
    // first change: without it, the first message reports the character's
    // entire accumulated reputation as the gained amount.
    int32_t before = 0;
    if (reputationIndex < standings_.size()) {
        before = standings_[reputationIndex].standing;
        standings_[reputationIndex].standing = standing;
    } else if (auto it = byFaction_.find(factionId); it != byFaction_.end()) {
        before = it->second;
    }
    byFaction_[factionId] = standing;
    const int32_t delta = standing - before;
    if (delta == 0) return {};
    watched_ = factionId;
    std::string name = catalog.name(factionId);
    if (name.empty()) name = "faction #" + std::to_string(factionId);
    return {StandingChanged{factionId, std::move(name), delta, standing}};
}

std::vector<ReputationChange> Reputation::setAtWar(uint32_t reputationIndex, bool atWar) {
    if (reputationIndex >= standings_.size()) return {};
    auto& flags = standings_[reputationIndex].flags;
    if (atWar) flags |= kFactionAtWar; else flags &= ~kFactionAtWar;
    dirty_ = true;
    return {AtWarChanged{reputationIndex, atWar}};
}

void Reputation::setVisible(uint32_t reputationIndex, bool visible) {
    if (reputationIndex >= standings_.size()) return;
    auto& flags = standings_[reputationIndex].flags;
    if (visible) flags |= kFactionVisible; else flags &= ~kFactionVisible;
    dirty_ = true;
}

bool Reputation::declareWar(uint32_t reputationIndex, bool atWar) {
    if (reputationIndex >= standings_.size()) return false;
    auto& flags = standings_[reputationIndex].flags;
    if (atWar && (flags & kFactionPeaceForced) != 0) return false;
    if (atWar) flags |= kFactionAtWar; else flags &= ~kFactionAtWar;
    dirty_ = true;
    return true;
}

bool Reputation::setInactive(uint32_t reputationIndex, bool inactive) {
    if (reputationIndex >= standings_.size()) return false;
    auto& flags = standings_[reputationIndex].flags;
    if (inactive) flags |= kFactionInactive; else flags &= ~kFactionInactive;
    dirty_ = true;
    return true;
}

bool Reputation::collapse(uint32_t factionId, bool collapsed) {
    if (factionId == 0) return false;
    const bool changed = collapsed ? collapsed_.insert(factionId).second : collapsed_.erase(factionId) > 0;
    // Rebuilding on a no-op would be harmless but the redraw is not: the
    // reputation panel reads its scroll position back from the row count.
    if (changed) dirty_ = true;
    return changed;
}

void Reputation::restoreCollapsed(std::unordered_set<uint32_t> factionIds) {
    collapsed_ = std::move(factionIds);
    dirty_ = true;
}

int32_t Reputation::standingOf(uint32_t factionId, const FactionCatalog& catalog) const {
    if (auto it = byFaction_.find(factionId); it != byFaction_.end()) return it->second;
    const uint32_t index = catalog.reputationIndexOf(factionId);
    return index < standings_.size() ? standings_[index].standing : 0;
}

const FactionStanding* Reputation::at(uint32_t reputationIndex) const {
    return reputationIndex < standings_.size() ? &standings_[reputationIndex] : nullptr;
}

const std::vector<ReputationEntry>& Reputation::list(const FactionCatalog& catalog) const {
    if (dirty_) rebuild(catalog);
    return list_;
}

const std::vector<ReputationRow>& Reputation::rows(const FactionCatalog& catalog) const {
    if (dirty_) rebuild(catalog);
    return rows_;
}

void Reputation::rebuild(const FactionCatalog& catalog) const {
    dirty_ = false;
    list_.clear();
    rows_.clear();

    for (size_t index = 0; index < standings_.size(); ++index) {
        const auto& s = standings_[index];
        // Hidden factions are never shown, and one the player has not met yet
        // is not visible either.
        if ((s.flags & kFactionHidden) != 0) continue;
        if ((s.flags & kFactionVisible) == 0) continue;
        const uint32_t factionId = catalog.factionAt(static_cast<uint32_t>(index));
        if (factionId == 0) continue;
        list_.push_back({factionId, static_cast<uint32_t>(index), catalog.name(factionId), s.flags});
    }
    if (list_.empty()) return;

    // Everything drawn: the visible factions, plus every faction one of them
    // descends from. A group whose members the player has not met is not drawn
    // at all, so its heading is not either.
    std::unordered_map<uint32_t, const ReputationEntry*> byId;
    std::unordered_set<uint32_t> displayed;
    for (const auto& e : list_) { byId[e.factionId] = &e; displayed.insert(e.factionId); }
    for (const auto& e : list_) {
        // Faction.dbc is data, and a cycle in it would spin here. The real
        // chain is three deep, so a small ceiling costs nothing.
        uint32_t p = catalog.parentOf(e.factionId);
        for (int depth = 0; p != 0 && depth < 8; ++depth) {
            displayed.insert(p);
            p = catalog.parentOf(p);
        }
    }

    // Where each group sits: at its first member's place in the server's list.
    // Ordering by the heading's own index instead would scatter the groups,
    // since Classic is 96 while the factions inside it start at 10.
    constexpr size_t kNoRank = static_cast<size_t>(-1);
    std::unordered_map<uint32_t, size_t> rank;
    for (uint32_t id : displayed) rank[id] = kNoRank;
    for (size_t i = 0; i < list_.size(); ++i) {
        uint32_t id = list_[i].factionId;
        for (int depth = 0; id != 0 && depth < 9; ++depth) {
            auto it = rank.find(id);
            if (it != rank.end() && i < it->second) it->second = i;
            id = catalog.parentOf(id);
        }
    }

    // The tree, with a parent outside the drawn set counting as no parent.
    std::unordered_map<uint32_t, std::vector<uint32_t>> children;
    for (uint32_t id : displayed) {
        const uint32_t parent = catalog.parentOf(id);
        children[displayed.count(parent) ? parent : 0u].push_back(id);
    }
    const auto byRank = [&](uint32_t a, uint32_t b) {
        if (rank[a] != rank[b]) return rank[a] < rank[b];
        return catalog.name(a) < catalog.name(b);
    };
    for (auto& [parent, kids] : children) {
        (void)parent;
        std::sort(kids.begin(), kids.end(), byRank);
    }

    // Depth first, so a heading is immediately followed by what is under it.
    // FrameXML draws a flat list and takes the nesting from isHeader/isChild,
    // which means the order *is* the grouping.
    std::vector<std::pair<uint32_t, int>> stack;
    for (uint32_t child : std::views::reverse(children[0])) stack.emplace_back(child, 0);
    while (!stack.empty()) {
        const auto [factionId, depth] = stack.back();
        stack.pop_back();

        auto kidsIt = children.find(factionId);
        const bool isHeader = kidsIt != children.end() && !kidsIt->second.empty();

        ReputationRow row;
        row.factionId = factionId;
        row.isHeader = isHeader;
        // Only two levels are drawn: FrameXML knows a heading and an indented
        // row and nothing deeper, so everything below the top shares one indent.
        row.isChild = depth > 0;
        if (auto entry = byId.find(factionId); entry != byId.end()) {
            row.reputationIndex = entry->second->reputationIndex;
            row.name = entry->second->name;
            row.flags = entry->second->flags;
            row.hasRep = true;
        } else {
            // A heading the player has no standing with - Classic and the two
            // expansion groups are the usual ones. Drawn as a bare heading.
            row.name = catalog.name(factionId);
        }
        rows_.push_back(std::move(row));

        // A closed group keeps its heading and loses everything under it: the
        // panel counts rows and indexes into them, so a hidden row must not be
        // in the list at all.
        if (!isHeader || isCollapsed(factionId)) continue;
        for (uint32_t child : std::views::reverse(kidsIt->second)) stack.emplace_back(child, depth + 1);
    }
}

} // namespace wowee::reputation

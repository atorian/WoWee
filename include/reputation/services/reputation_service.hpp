#pragma once

#include "reputation/domain/factions.hpp"
#include "reputation/domain/records.hpp"
#include "reputation/domain/standings.hpp"
#include "reputation/services/ports.hpp"
#include "reputation/services/reputation.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::reputation {

class ReputationService final : public IReputation, public IReputationFeed {
public:
    ReputationService(IFactionData& data, IReputationGateway& gateway, core::EventBus& events);

    [[nodiscard]] const std::vector<ReputationEntry>& getReputationList() const override;
    [[nodiscard]] const std::vector<ReputationRow>& getReputationRows() const override;
    [[nodiscard]] int32_t getFactionStanding(uint32_t factionId) const override;
    [[nodiscard]] const std::unordered_map<uint32_t, int32_t>& getFactionStandings() const override { return player_.byFaction(); }
    [[nodiscard]] const FactionStanding* getStandingAt(uint32_t reputationIndex) const override { return player_.at(reputationIndex); }
    [[nodiscard]] const std::string& getFactionName(uint32_t factionId) const override;
    [[nodiscard]] uint32_t getFactionIdByRepListId(uint32_t reputationIndex) const override;

    [[nodiscard]] bool isFactionCollapsed(uint32_t factionId) const override { return player_.isCollapsed(factionId); }
    void setFactionCollapsed(uint32_t factionId, bool collapsed) override { player_.collapse(factionId, collapsed); }
    [[nodiscard]] const std::unordered_set<uint32_t>& getCollapsedFactions() const override { return player_.collapsed(); }
    void restoreCollapsedFactions(std::unordered_set<uint32_t> factionIds) override { player_.restoreCollapsed(std::move(factionIds)); }

    [[nodiscard]] uint32_t getWatchedFactionId() const override { return player_.watched(); }
    void setWatchedFactionId(uint32_t factionId) override;
    void setFactionAtWar(uint32_t reputationIndex, bool atWar) override;
    void setFactionInactive(uint32_t reputationIndex, bool inactive) override;
    void loadFactions() override { ensureLoaded(); }

    void onInitialize(std::vector<FactionStanding> earned) override;
    void onStanding(uint32_t reputationIndex, int32_t earned) override;
    void onAtWar(uint32_t reputationIndex, bool atWar) override;
    void onVisible(uint32_t reputationIndex, bool visible) override;
    void beginCharacter() override { player_.beginCharacter(); }

private:
    /// Faction.dbc, latched only once it can be read: a caller can reach this
    /// before the assets are up, and one early call would otherwise disable
    /// the file for the whole session.
    void ensureLoaded() const;
    int32_t whole(uint32_t factionId, int32_t earned) const;
    void publish(const std::vector<ReputationChange>& changes);

    IFactionData& data_;
    IReputationGateway& gateway_;
    core::EventBus& events_;
    mutable FactionCatalog catalog_;
    mutable bool loaded_ = false;
    mutable Reputation player_;
};

} // namespace wowee::reputation

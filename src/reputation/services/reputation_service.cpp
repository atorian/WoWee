#include "reputation/services/reputation_service.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"

namespace wowee::reputation {

namespace {

class NoReputation final : public IReputation {
public:
    const std::vector<ReputationEntry>& getReputationList() const override { return list_; }
    const std::vector<ReputationRow>& getReputationRows() const override { return rows_; }
    int32_t getFactionStanding(uint32_t) const override { return 0; }
    const std::unordered_map<uint32_t, int32_t>& getFactionStandings() const override { return standings_; }
    const FactionStanding* getStandingAt(uint32_t) const override { return nullptr; }
    const std::string& getFactionName(uint32_t) const override { return name_; }
    uint32_t getFactionIdByRepListId(uint32_t) const override { return 0; }
    bool isFactionCollapsed(uint32_t) const override { return false; }
    void setFactionCollapsed(uint32_t, bool) override {}
    const std::unordered_set<uint32_t>& getCollapsedFactions() const override { return collapsed_; }
    void restoreCollapsedFactions(std::unordered_set<uint32_t>) override {}
    uint32_t getWatchedFactionId() const override { return 0; }
    void setWatchedFactionId(uint32_t) override {}
    void setFactionAtWar(uint32_t, bool) override {}
    void setFactionInactive(uint32_t, bool) override {}
    void loadFactions() override {}

private:
    std::vector<ReputationEntry> list_;
    std::vector<ReputationRow> rows_;
    std::unordered_map<uint32_t, int32_t> standings_;
    std::string name_;
    std::unordered_set<uint32_t> collapsed_;
};

NoReputation kNoReputation;

} // namespace

IReputation& orNoReputation(IReputation* reputation) {
    return reputation ? *reputation : kNoReputation;
}

ReputationService::ReputationService(IFactionData& data, IReputationGateway& gateway, core::EventBus& events)
    : data_(data), gateway_(gateway), events_(events) {}

void ReputationService::ensureLoaded() const {
    if (loaded_ || !data_.ready()) return;
    loaded_ = true;
    catalog_.describe(data_.factions());
    player_.catalogChanged();
}

const std::vector<ReputationEntry>& ReputationService::getReputationList() const {
    ensureLoaded();
    return player_.list(catalog_);
}

const std::vector<ReputationRow>& ReputationService::getReputationRows() const {
    ensureLoaded();
    return player_.rows(catalog_);
}

int32_t ReputationService::getFactionStanding(uint32_t factionId) const {
    ensureLoaded();
    return player_.standingOf(factionId, catalog_);
}

const std::string& ReputationService::getFactionName(uint32_t factionId) const {
    ensureLoaded();
    return catalog_.name(factionId);
}

uint32_t ReputationService::getFactionIdByRepListId(uint32_t reputationIndex) const {
    ensureLoaded();
    return catalog_.factionAt(reputationIndex);
}

void ReputationService::setWatchedFactionId(uint32_t factionId) {
    player_.watch(factionId);
    if (!gateway_.online()) return;
    int32_t index = -1;
    if (factionId != 0) {
        ensureLoaded();
        const uint32_t i = catalog_.reputationIndexOf(factionId);
        if (i != kNoReputationIndex) index = static_cast<int32_t>(i);
    }
    gateway_.sendWatched(index);
}

void ReputationService::setFactionAtWar(uint32_t reputationIndex, bool atWar) {
    if (!player_.declareWar(reputationIndex, atWar)) return;
    if (gateway_.online()) gateway_.sendAtWar(reputationIndex, atWar);
}

void ReputationService::setFactionInactive(uint32_t reputationIndex, bool inactive) {
    // No confirmation comes back for this one, so the flag is the answer.
    if (!player_.setInactive(reputationIndex, inactive)) return;
    if (gateway_.online()) gateway_.sendInactive(reputationIndex, inactive);
}

int32_t ReputationService::whole(uint32_t factionId, int32_t earned) const {
    return earned + catalog_.baseStanding(factionId, gateway_.playerRace(), gateway_.playerClass());
}

void ReputationService::onInitialize(std::vector<FactionStanding> earned) {
    ensureLoaded();
    // Each made whole: the packet carries what has been earned, and the
    // standing is that on top of where the faction starts for this race and
    // class. Without it every faction with a starting value came out wrong.
    for (size_t i = 0; i < earned.size(); ++i) {
        const uint32_t factionId = catalog_.factionAt(static_cast<uint32_t>(i));
        if (factionId != 0) earned[i].standing = whole(factionId, earned[i].standing);
    }
    const size_t count = earned.size();
    publish(player_.initialize(std::move(earned), catalog_));
    LOG_INFO("Reputation: ", count, " factions initialised, ", player_.byFaction().size(),
             " resolved to a faction id");
}

void ReputationService::onStanding(uint32_t reputationIndex, int32_t earned) {
    ensureLoaded();
    const uint32_t factionId = catalog_.factionAt(reputationIndex);
    if (factionId == 0) {
        LOG_WARNING("SMSG_SET_FACTION_STANDING: unknown repListId=", reputationIndex,
                    " standing=", earned, "; that standing is not shown");
        return;
    }
    publish(player_.changeStanding(reputationIndex, factionId, whole(factionId, earned), catalog_));
}

void ReputationService::onAtWar(uint32_t reputationIndex, bool atWar) {
    publish(player_.setAtWar(reputationIndex, atWar));
}

void ReputationService::publish(const std::vector<ReputationChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

} // namespace wowee::reputation

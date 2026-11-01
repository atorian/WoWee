#pragma once

#include <cstdint>
#include <vector>

#include "reputation/domain/factions.hpp"

namespace wowee::reputation {

/// Faction.dbc. Implemented over the asset manager.
class IFactionData {
public:
    virtual ~IFactionData() = default;

    /// Whether the files can be read yet.
    [[nodiscard]] virtual bool ready() const = 0;
    /// No rows when the file is missing.
    virtual std::vector<FactionRecord> factions() = 0;
};

/// Requests to the server about reputation, and who the player is. Implemented
/// by the protocol layer.
class IReputationGateway {
public:
    virtual ~IReputationGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    [[nodiscard]] virtual uint8_t playerRace() const = 0;
    [[nodiscard]] virtual uint8_t playerClass() const = 0;
    /// CMSG_SET_WATCHED_FACTION: -1 watches nothing.
    virtual void sendWatched(int32_t reputationIndex) = 0;
    virtual void sendAtWar(uint32_t reputationIndex, bool atWar) = 0;
    virtual void sendInactive(uint32_t reputationIndex, bool inactive) = 0;
};

} // namespace wowee::reputation

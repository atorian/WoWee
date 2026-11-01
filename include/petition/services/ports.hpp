#pragma once

#include <cstdint>
#include <string>

namespace wowee::petition {

/// Requests to the server about charters. Implemented by the protocol layer.
class IPetitionGateway {
public:
    virtual ~IPetitionGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// A connection, in the world or not.
    [[nodiscard]] virtual bool connected() const = 0;

    virtual void sendBuy(uint64_t npcGuid, const std::string& name, uint32_t clientIndex) = 0;
    virtual void sendSign(uint64_t petitionGuid) = 0;
    virtual void sendTurnIn(uint64_t petitionGuid) = 0;
    virtual void sendOffer(uint64_t petitionGuid, uint64_t targetGuid) = 0;
    virtual void sendRename(uint64_t petitionGuid, const std::string& name) = 0;
    /// Ask what a charter is: its name, how many signatures it needs, and
    /// whether it is a guild's or an arena team's.
    virtual void sendQuery(uint32_t entry, uint64_t petitionGuid) = 0;
};

} // namespace wowee::petition

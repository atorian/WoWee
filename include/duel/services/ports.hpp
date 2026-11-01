#pragma once

#include <cstdint>
#include <string>

namespace wowee::duel {

/// Requests to the server about duels. Implemented by the protocol layer.
class IDuelGateway {
public:
    virtual ~IDuelGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// Both answers name the duel flag.
    virtual void sendAccept(uint64_t arbiter) = 0;
    virtual void sendCancel(uint64_t arbiter) = 0;
    virtual void sendPropose(uint64_t target) = 0;
};

/// What duels need from the rest of the game.
class IDuelWorld {
public:
    virtual ~IDuelWorld() = default;

    [[nodiscard]] virtual uint64_t playerGuid() const = 0;
    /// The unit's name if it is in view, else the cached player name, else
    /// empty.
    [[nodiscard]] virtual std::string name(uint64_t guid) const = 0;
};

} // namespace wowee::duel

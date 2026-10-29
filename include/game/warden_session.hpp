#pragma once

#include <cstdint>
#include <vector>

namespace wowee::game {

struct KnownTitleUpdate;

/// What WardenHandler needs of the session besides sending: who this client
/// told the realm it is, the key the login agreed, and where a realm
/// extension's reports about the character go.
class WardenSession {
public:
    virtual ~WardenSession() = default;
    /// The client build sent in CMSG_AUTH_SESSION.
    [[nodiscard]] virtual uint32_t clientBuild() const = 0;
    /// The SRP session key the login server agreed: 40 bytes once
    /// authenticated, empty before.
    [[nodiscard]] virtual const std::vector<uint8_t>& loginSessionKey() const = 0;
    /// A realm extension said how much money the character holds, in copper.
    virtual void serverMoneyReported(uint64_t copper) = 0;
    /// A realm extension changed the titles the character knows.
    virtual void knownTitlesReported(const KnownTitleUpdate& update) = 0;
};

} // namespace wowee::game

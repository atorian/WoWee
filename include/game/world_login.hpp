#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

/// What a login to a world server is made with: where the realm is, the
/// session the login server gave, and how this client introduces itself.
/// GameHandler keeps the last one, so the connection can be made again after
/// it is lost - with the realm's address as the login server gives it then.
struct WorldLogin {
    std::string host;
    uint16_t port = 0;
    /// 40 bytes; the secret the world handshake proves and the header cipher
    /// is keyed from. Never logged.
    std::vector<uint8_t> sessionKey;
    std::string account;
    uint32_t build = 0;
    uint32_t realmId = 0;
    /// The login server, which server extensions are looked up by. Empty for
    /// the world server's own host.
    std::string extensionHost;
    uint16_t extensionPort = 0;
};

} // namespace wowee::game

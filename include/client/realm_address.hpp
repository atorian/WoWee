#pragma once

#include <cstdint>
#include <string>

namespace wowee::core {

/// Where a realm's world server is, as the client connects to it.
struct RealmAddress {
    std::string host;
    uint16_t port = 8085;
};

/// A realm list address, "host:port", as the client connects to it: the port
/// defaults to 8085, and WOWEE_REALM_HOST_OVERRIDE replaces the host - a LAN
/// client handed a public address its router cannot hairpin back to. Both the
/// realm screen's connect and a reconnect read the address through here.
RealmAddress resolveRealmAddress(const std::string& address);

} // namespace wowee::core

#include "core/realm_address.hpp"

#include "core/logger.hpp"

#include <cstdlib>

namespace wowee::core {

RealmAddress resolveRealmAddress(const std::string& address) {
    RealmAddress out;
    out.host = address;
    const size_t colon = address.find(':');
    if (colon != std::string::npos) {
        out.host = address.substr(0, colon);
        try {
            out.port = static_cast<uint16_t>(std::stoi(address.substr(colon + 1)));
        } catch (...) {
            LOG_WARNING("Invalid port in realm address '", address, "'; using ", out.port);
        }
    }
    // LAN clients are often handed a public realm address by MaNGOS. On
    // routers without NAT reflection that address cannot hairpin back to the
    // local world server. Allow a client-side host override while retaining
    // the realm-advertised port and authentication identity.
    if (const char* overrideHost = std::getenv("WOWEE_REALM_HOST_OVERRIDE");
        overrideHost && *overrideHost) {
        LOG_WARNING("Overriding realm host '", out.host, "' with '", overrideHost,
                    "' via WOWEE_REALM_HOST_OVERRIDE");
        out.host = overrideHost;
    }
    return out;
}

} // namespace wowee::core

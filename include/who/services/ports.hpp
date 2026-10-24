#pragma once

#include <string>

namespace wowee::who {

/// Requests to the server about /who. Implemented by the protocol layer.
class IWhoGateway {
public:
    virtual ~IWhoGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    virtual void sendQuery(const std::string& playerName) = 0;
};

} // namespace wowee::who

#pragma once

// Domain interfaces for GameHandler decomposition (Phase 1.2A).
// Each interface defines a narrow contract for a specific domain concern,
// enabling domain handlers to depend only on the state they need.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>

namespace wowee::network { class WorldConnection; }

namespace wowee::game {

// Forward declarations
enum class WorldState;

// ---------------------------------------------------------------------------
// IConnectionState - server connection and authentication state
// ---------------------------------------------------------------------------
class IConnectionState {
public:
    virtual ~IConnectionState() = default;

    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual bool isInWorld() const = 0;
    [[nodiscard]] virtual WorldState getState() const = 0;
    virtual network::WorldConnection* getSocket() = 0;
    [[nodiscard]] virtual const std::vector<uint8_t>& getSessionKey() const = 0;
};

} // namespace wowee::game

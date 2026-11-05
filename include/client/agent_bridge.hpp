#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace wowee {
namespace rendering { class Renderer; class CameraController; }
namespace game { class GameHandler; }
namespace ui { class AgentControls; }
namespace audio { class AudioCoordinator; }
namespace core {

/// The client's MCP endpoint: an agent on this machine reads the game's state,
/// takes screenshots and - once the player hands control over with /ai -
/// walks, turns and looks.
///
/// Off unless WOWEE_MCP_PORT names a port. Listens on 127.0.0.1 only, and every
/// request needs the bearer token kept in <config root>/mcp_token, created on
/// first use and readable by this user alone.
class AgentBridge {
public:
    struct Dependencies {
        rendering::Renderer* renderer = nullptr;
        game::GameHandler* gameHandler = nullptr;
        ui::AgentControls* controls = nullptr;
        rendering::CameraController* camera = nullptr;
        /// Names the zone the player is in.
        const audio::AudioCoordinator* audio = nullptr;
    };

    explicit AgentBridge(Dependencies dependencies);
    ~AgentBridge();

    bool start(uint16_t port, std::string& error);
    /// Main thread, once a frame: runs the tool calls waiting for it.
    void pump(float deltaTime);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
} // namespace wowee

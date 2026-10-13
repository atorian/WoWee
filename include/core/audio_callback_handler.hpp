#pragma once

#include "core/event_bus.hpp"
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace pipeline { class AssetManager; }
namespace ui { class UIManager; }

namespace core {

class PlayerView;

/// Handles audio-related callbacks: music, sound effects, level-up, achievement, LFG.
class AudioCallbackHandler {
public:
    AudioCallbackHandler(pipeline::AssetManager& assetManager,
                         audio::AudioCoordinator* audioCoordinator,
                         rendering::Renderer* renderer,
                         PlayerView* playerView,
                         ui::UIManager* uiManager,
                         game::GameHandler& gameHandler);

    void setupCallbacks();

private:
    /// Resolve SoundEntries.dbc → file path for a given soundId (eliminates 3x copy-paste)
    [[nodiscard]] std::optional<std::string> resolveSoundEntryPath(uint32_t soundId) const;

    pipeline::AssetManager& assetManager_;
    audio::AudioCoordinator* audioCoordinator_;
    rendering::Renderer* renderer_;
    PlayerView* playerView_;
    ui::UIManager* uiManager_;
    game::GameHandler& gameHandler_;
    std::vector<EventBus::Subscription> subscriptions_;
};

} // namespace core
} // namespace wowee

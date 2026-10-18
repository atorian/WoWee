#pragma once

#include <functional>

namespace wowee {

// Forward declarations
namespace core { 
    class Window;
    class EntitySpawner;
    class AppearanceComposer;
    class WorldLoader;
    class ZoneTracker;
    class ScreenRecording;
}
namespace rendering {
class CpuStageTimes;
class Renderer;
class CameraController;
class AnimationController;
namespace world_map { class WorldMapFacade; }
}
namespace pipeline { class AssetManager; }
namespace game { 
    class GameHandler;
    class ExpansionRegistry;
}
namespace addons { class AddonManager; class AddonCatalog; }
namespace audio { class AudioCoordinator; }

namespace ui {

/**
 * UI Services - Dependency injection container for UI components.
 * 
 * Break the singleton Phase B
 * 
 * Replaces Application::getInstance() calls throughout UI code.
 * Application creates this struct and injects it into UIManager,
 * which propagates it to GameScreen and all child UI components.
 * 
 * Owned by Application, shared as const pointers (non-owning).
 */
struct UIServices {
    core::Window* window = nullptr;
    rendering::Renderer* renderer = nullptr;
    pipeline::AssetManager* assetManager = nullptr;
    game::GameHandler* gameHandler = nullptr;
    game::ExpansionRegistry* expansionRegistry = nullptr;
    /// The interface of the character in the world, in whatever state it is;
    /// null with no character in the world. Asked each time rather than held:
    /// each character has its own, and a /reload replaces it.
    std::function<addons::AddonManager*()> addonManager;
    [[nodiscard]] addons::AddonManager* characterAddons() const {
        return addonManager ? addonManager() : nullptr;
    }
    /// The addons on disk and which are switched on - the player's, for the
    /// whole run. What the character screen's AddOns list shows and toggles.
    addons::AddonCatalog* addonCatalog = nullptr;
    /// Ask for the character's interface to be rebuilt between frames, as
    /// ReloadUI() does. Performed by the session that owns the interface.
    std::function<void()> requestInterfaceReload;
    audio::AudioCoordinator* audioCoordinator = nullptr;
    core::ZoneTracker* zoneTracker = nullptr;
    core::ScreenRecording* screenRecording = nullptr;
    rendering::CameraController* cameraController = nullptr;
    rendering::AnimationController* animationController = nullptr;
    /// The world map, which is built on entering the world: null before.
    std::function<rendering::world_map::WorldMapFacade*()> worldMap;
    /// Where the interface's own parts record what they cost this frame, for
    /// the frame profile (CpuStageLaps); null records nothing.
    rendering::CpuStageTimes* interfaceStages = nullptr;
    
    // Extracted classes (also available individually for Phase A compatibility)
    core::EntitySpawner* entitySpawner = nullptr;
    core::AppearanceComposer* appearanceComposer = nullptr;
    core::WorldLoader* worldLoader = nullptr;
    
    // Helper to check if core services are wired
    [[nodiscard]] bool isValid() const {
        return window && renderer && assetManager && gameHandler;
    }
};

} // namespace ui
} // namespace wowee

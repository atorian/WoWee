#pragma once

#include "addons/lua_services.hpp"
#include "core/character_session.hpp"
#include "core/event_bus.hpp"
#include "core/frame_plan.hpp"
#include "core/interface_input.hpp"
#include "core/world_load_progress.hpp"
#include "core/world_loader.hpp"
#include "core/world_reconnect.hpp"
#include "core/world_transition_queue.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace wowee {
namespace addons { class AddonCatalog; class AddonManager; }
namespace audio { class AudioCoordinator; }
namespace game { class GameHandler; }
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; class SpellVisualSystem; }

namespace core {

class AnimationCallbackHandler;
class AppearanceComposer;
class EntitySpawner;
class FrameObserver;
class MapScope;
class PlayerView;
class WorldEntryCallbackHandler;

/// The window the client runs in, as its frame sees it.
class ClientWindow {
public:
    virtual ~ClientWindow() = default;
    [[nodiscard]] virtual bool shouldClose() const = 0;
    /// The player, or the client itself, closes the window: the loop ends
    /// before the next phase that would act on the frame.
    virtual void requestClose() = 0;
    /// The start of a frame: the seconds since the last one began.
    virtual float beginFrame() = 0;
    /// Pace: hold the frame to the display's cap.
    virtual void pace() = 0;
};

/// Where the Input phase reads the player from.
class ClientInput {
public:
    virtual ~ClientInput() = default;
    /// Input: take what the platform delivered since the last frame. A request
    /// to close goes to the window; what the interface hears - keys, text,
    /// the wheel, focus changes - goes to `forInterface`, which the Interface
    /// phase delivers.
    virtual void pump(float deltaTime, const FramePlan& plan, InterfaceInput& forInterface) = 0;
};

/// What the frame drives but does not own: the renderer and everything that
/// draws through it, the screens, the sound, the login server. Application is
/// the real one. A client that draws nothing - ClientHarness - has none of it,
/// and every one of these does nothing there.
///
/// Each is called from one place in ClientLoop, named for when.
class ClientFrontEnd {
public:
    virtual ~ClientFrontEnd() = default;

    // ---- Each frame, in phase order ----------------------------------------

    /// Network, before the world connection is read: the login server and the
    /// realm's patch archives.
    virtual void updateLogin(float deltaTime, const FramePlan& plan) = 0;
    /// Game, first: the offline fixtures' own per-frame setup.
    virtual void prepareFrame() = 0;
    /// Game, in the world: everything the client presents of it - the camera's
    /// rules, the models kept on their entities, the markers.
    virtual void updateWorld(float deltaTime) = 0;
    /// Streaming, first: the models a loading map builds.
    virtual void streamScene(float deltaTime, const FramePlan& plan) = 0;
    /// Streaming, while loading: how far the load has come.
    virtual void showLoadProgress(const WorldLoadStatus& status) = 0;
    /// Interface, after the input has reached the interface: the pad's key
    /// names and the pad's own bindings.
    virtual void afterInterfaceInput() = 0;
    /// Interface: whether there is a surface to build the screens on.
    [[nodiscard]] virtual bool canDrawInterface() const = 0;
    /// Interface: the screens' own update, before a requested interface reload.
    virtual void updateScreens(float deltaTime) = 0;
    /// Interface: the screens built into the frame.
    virtual void drawInterface(float deltaTime, const FramePlan& plan) = 0;
    /// Interface: a key nothing in the interface took (the development keys).
    virtual void keyForClient(const SDL_Event& event) = 0;
    /// Interface: one of the client's own shortcuts was pressed. The press
    /// went nowhere else - see clientShortcutFor.
    virtual void clientShortcut(ClientShortcut shortcut) = 0;
    /// Present, first: the changes asked of the scene since the last frame -
    /// spawns, removals, transports moved, animations - made, so the player's
    /// controls and everything after read this frame's world. The renderers'
    /// instances change only inside Present (rendering::SceneCommands).
    virtual void applySceneChanges() = 0;
    /// Present: the world handed to the renderer, after the player's controls
    /// have moved the camera.
    virtual void present(float deltaTime, const FramePlan& plan) = 0;
    /// Present, while loading: whether models for the scene are still being
    /// built, which holds the load in its warmup.
    [[nodiscard]] virtual bool scenePending() const = 0;
    /// Render: the frame recorded and submitted.
    virtual void render(const FramePlan& plan) = 0;
    /// Transitions, last: whether the device survived the frame.
    virtual void afterTransitions() = 0;

    // ---- The frame budget (WOWEE_FRAME_PROFILE) -----------------------------

    [[nodiscard]] virtual bool profilingStages() const = 0;
    virtual void noteStageTime(const char* stage, float milliseconds) = 0;
    virtual void frameFinished() = 0;

    // ---- Transitions ---------------------------------------------------------

    /// A map's load has begun: where the character is drawn, the loading screen.
    virtual void mapEntryBegun(const WorldLoader::Entry& entry) = 0;
    /// The map's renderers exist and its scope has taken them: the player's
    /// model, the world's sounds, the map's name in every view of it.
    virtual void mapRenderersReady(const WorldLoader::Entry& entry, const std::string& mapName) = 0;
    /// An instance's geometry is up: the player is put on its floor, before
    /// the camera is put back behind them.
    virtual void placeOnInstanceFloor(const WorldLoader::Entry& entry) = 0;
    /// The map's geometry is up: the character's facing, the portals drawn at
    /// its doors.
    virtual void mapGeometryReady(const WorldLoader::Entry& entry) = 0;
    /// The world is entered: what the presentation hangs on the game for it.
    virtual void enteredWorld() = 0;
    /// The character screen is shown.
    virtual void characterScreenShown() = 0;
    /// The character has left the world; the screens they return to.
    virtual void characterLeft(bool keptConnection) = 0;
    /// Why the player is back at the login screen, when it was not their idea.
    virtual void showDisconnectNotice(const std::string& notice) = 0;
    /// Interface, each frame: what to say over the world while the client
    /// reconnects ("Connection lost. Reconnecting (attempt 2 of 5)..."), empty
    /// when it is not.
    virtual void showReconnectStatus(const std::string& status) = 0;
    /// The client is about to close after an exit the server confirmed.
    virtual void exiting() = 0;
};

/// The client's frame and the transitions it applies.
///
/// One frame is Input, Network, Game, Events, Streaming, Interface, Present,
/// Render, Transitions, Pace (docs/principles/client-frame-phases.md), from one
/// plan chosen by the application state before Input. The world's lifetimes -
/// the character's CharacterSession and the MapScope inside it - begin and end
/// here and only in Transitions, after the frame is submitted
/// (docs/principles/world-transition-boundary.md).
///
/// It is given the window, the input and the front end as interfaces, and the
/// renderer as a pointer that is null in a client that draws nothing, so the
/// same frame and the same transitions run in Application and in a test with
/// none of them (tests/client_harness.hpp).
class ClientLoop {
public:
    struct Services {
        Phase& phase;
        EventBus& events;
        game::GameHandler& game;
        ClientWindow& window;
        ClientInput& input;
        ClientFrontEnd& frontEnd;
        /// The local player's camera and controls. Exists without a renderer.
        PlayerView& playerView;
        /// What a world entry needs. Null when the game data did not load; a
        /// world entry is then refused, and said so.
        WorldLoader* loader = nullptr;
        EntitySpawner* spawner = nullptr;
        WorldEntryCallbackHandler* worldEntry = nullptr;
        /// Null in a client that draws nothing.
        rendering::Renderer* renderer = nullptr;
        rendering::SpellVisualSystem* spellVisuals = nullptr;
        audio::AudioCoordinator* audio = nullptr;
        pipeline::AssetManager* assets = nullptr;
        /// Null when the addon system could not start.
        addons::AddonCatalog* addonCatalog = nullptr;
        /// What each character's interface is handed to reach the client.
        addons::LuaServices luaServices;
        /// How a lost session is renewed without the player. Null: a lost
        /// world connection ends the character, back at the login screen.
        WorldSessionRenewal* renewal = nullptr;
        AppearanceComposer* appearance = nullptr;
        AnimationCallbackHandler* animationCallbacks = nullptr;
        /// How much of an interface each character gets.
        CharacterSession::Interface sessionInterface = CharacterSession::Interface::Full;
    };

    ClientLoop(Services services, AppState initialState);
    /// Ends the character's session, if one stands, and lets go of the game's
    /// FrameXML channel.
    ~ClientLoop();

    ClientLoop(const ClientLoop&) = delete;
    ClientLoop& operator=(const ClientLoop&) = delete;

    /// One frame, in phase order.
    void runFrame();
    /// The loop that ran the frames has ended; the observer hears it.
    void loopEnded();
    /// Who watches the frames: a native fixture or a test. None by default.
    void setObserver(std::unique_ptr<FrameObserver> observer);

    [[nodiscard]] AppState state() const { return state_; }
    /// A state change outside Transitions waits for the next Transitions.
    void requestState(AppState state);

    /// The character in the world, in any state; null with none.
    [[nodiscard]] CharacterSession* session() { return session_.get(); }
    /// The map the character is on; null between maps.
    [[nodiscard]] MapScope* currentMap() { return session_ ? session_->map() : nullptr; }
    /// The character's interface, in any state; null without one.
    [[nodiscard]] addons::AddonManager* characterAddons();
    /// The same, only while it hears the world.
    [[nodiscard]] addons::AddonManager* worldInterface();
    /// Whether the character is on their way out of the world: a logout,
    /// disconnect or exit waits for the next Transitions. A lost connection
    /// the client rides out in the world is not one.
    [[nodiscard]] bool leavingWorld() const { return worldEnding(); }
    /// The world connection is being restored while the character stays in
    /// the world: null when it is not.
    [[nodiscard]] const WorldReconnect* reconnecting() const { return reconnect_.get(); }

    /// The kind of interface the next character gets (the offline loading
    /// fixture's character has no unit fields for FrameXML).
    void setSessionInterface(CharacterSession::Interface kind) { services_.sessionInterface = kind; }

    /// An offline scene with no character and nothing to load: the sky,
    /// fountain and glass probes. Transitions only.
    void enterScene(uint32_t mapId);

    /// Shutdown, first: the interface hears the character leave while the
    /// world still stands.
    void leaveWorldForShutdown();
    /// Shutdown: the character's session ends, its map first.
    void endCharacter();

private:
    void beginPhase(Phase phase);
    bool requireTransitions(const char* operation) const;
    void setState(AppState state);

    /// The state's plan, with the world hidden once the logout is committed.
    [[nodiscard]] FramePlan framePlan() const;
    /// A committed logout the server never completes would leave the player
    /// before a hidden world for good: after this long the client leaves
    /// without it.
    static constexpr float kLogoutCompleteTimeoutSeconds = 10.0f;
    /// Game phase: give up on a committed logout the server has not completed.
    void watchCommittedLogout(float deltaTime);

    void dispatchNetwork(float deltaTime, const FramePlan& plan);
    void updateGame(float deltaTime, const FramePlan& plan);
    void updateStreaming(float deltaTime, const FramePlan& plan);
    void updateInterface(float deltaTime, const FramePlan& plan);
    void syncPresentation(float deltaTime, const FramePlan& plan);
    void processWorldTransitions();

    void beginWorldLoading(WorldLoader::Entry entry);
    void prepareWorldPlayer();
    void finishWorldGeometry();
    void completeWorldLoading();
    void performLogoutToLogin(bool keepConnection);
    /// Starts the session of the character the game has logged in, when none
    /// stands. Transitions only.
    void beginCharacterSession();
    /// Tell the interface the character is leaving, while the world stands.
    void leaveWorldInterface();
    /// Whether this frame's Interface phase is the last the character's
    /// interface hears the world: the world ends, or the character is back
    /// in it after a reconnect and gets an interface anew.
    [[nodiscard]] bool worldEndsThisFrame(const FramePlan& plan) const;
    /// Whether a queued leave ends the world. A lost connection does not
    /// while the client can reconnect with the character standing in it.
    [[nodiscard]] bool worldEnding() const;
    /// The character is back in the world after a reconnect and the entry
    /// waits for Transitions.
    [[nodiscard]] bool reentryPending() const;
    /// Whether a lost connection can be ridden out in the world: a character
    /// on a map, and a way to renew the session.
    [[nodiscard]] bool keepsWorldThroughReconnect(uint64_t characterGuid) const;

    /// The connection is lost in the world: the world stays, what is drawn of
    /// it is held, and the game logs the character in again. Transitions only.
    void beginReconnecting(uint64_t characterGuid);
    /// Game: the attempts, and giving up on them.
    void updateReconnect(float deltaTime);
    /// The character is back in the world: on the same map the next character
    /// session takes it; on another, the entry is an ordinary map load.
    /// Transitions only.
    void reenterWorld(const game::events::WorldEntered& entry);

    CharacterSession::Services characterSessionServices();
    MapScope::Services mapScopeServices();

    Services services_;
    AppState state_;
    std::optional<AppState> requestedState_;
    WorldTransitionQueue transitions_;
    InterfaceInput interfaceInput_;
    std::unique_ptr<FrameObserver> observer_;
    std::chrono::steady_clock::time_point phaseStartedAt_{};
    float networkUpdateMs_ = 0.0f;
    /// Why the player is back at the login screen, when it was not their idea.
    std::string disconnectNotice_;
    /// A world entry refused for want of a loader is said once.
    bool reportedNoLoader_ = false;
    /// While the world connection is restored, the character standing in the
    /// world; null otherwise.
    std::unique_ptr<WorldReconnect> reconnect_;
    /// After a reconnect onto the same map: frame time left before the
    /// entities the server did not show again are released; negative when
    /// nothing is held.
    float heldReleaseIn_ = -1.0f;
    /// The character in the world. Last, so it goes before everything else
    /// here; the destructor ends it explicitly.
    std::unique_ptr<CharacterSession> session_;
};

} // namespace core
} // namespace wowee

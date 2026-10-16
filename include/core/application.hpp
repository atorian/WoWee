#pragma once

#include "core/window.hpp"
#include <chrono>
#include "core/gamepad.hpp"
#include "ui/unit_portrait.hpp"
#include "ui/widget_renderer.hpp"
#include "core/input.hpp"
#include "core/entity_spawner.hpp"
#include "core/appearance_composer.hpp"
#include "core/world_loader.hpp"
#include "core/map_scope.hpp"
#include "core/character_session.hpp"
#include "core/client_loop.hpp"
#include "addons/lua_services.hpp"
#include "game/character.hpp"
#include "core/event_bus.hpp"
#include "core/frame_plan.hpp"
#include "core/frame_pacer.hpp"
#include "core/cvar_store.hpp"
#include "core/config_paths.hpp"
#include "game/game_services.hpp"
#include "game/reported_movement.hpp"
#include "pipeline/asset_inventory.hpp"
#include "core/update_check.hpp"
#include "pipeline/blp_loader.hpp"
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <optional>
#include <future>
#include <mutex>
#include <thread>
#include <atomic>

namespace wowee {

// Forward declarations
namespace rendering {
class Renderer;
class PerformanceHUD;
class CameraController;
class AnimationController;
class SpellVisualSystem;
namespace world_map { class WorldMapFacade; }
using WorldMap = world_map::WorldMapFacade;
}
namespace ui { class UIManager; class MapWindow; }
namespace auth { class AuthHandler; }
namespace game { class GameHandler; class ExpansionRegistry; struct ExpansionProfile; struct EzWowPatch; }
namespace pipeline { class AssetManager; class ArchiveLayer; class DBCLayout; struct M2Model; struct WMOModel; }
namespace audio { enum class VoiceType; class AudioCoordinator; }
namespace addons { class AddonManager; class AddonCatalog; class LuaErrorReport; }

namespace core {

// Handler forward declarations
class NPCInteractionCallbackHandler;
class LoginServerRenewal;
class ZoneTracker;
class PlayerView;
class SpellPresentation;
class RendererTransportScene;
class ScreenRecording;
class ResourceMonitor;
class AudioCallbackHandler;
class EntitySpawnCallbackHandler;
class AnimationCallbackHandler;
class TransportCallbackHandler;
class WorldEntryCallbackHandler;
class UIScreenCallbackHandler;
class RealmPatchSync;
class FrameObserver;

/// The client: the window, the renderer, the screens, the sound and the game,
/// and the frame that runs them (ClientLoop).
///
/// ClientLoop owns the frame's order and the world transitions; this is what
/// it drives - the window it runs in, the platform input it reads and the
/// front end it presents through - and everything those need.
class Application final : private ClientWindow, private ClientInput, private ClientFrontEnd {

public:
    Application();
    ~Application() override;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool initialize();
    void run();
    void shutdown();

    /// What watches the frames, for a program that is a fixture: set after
    /// initialize, before run. The ordinary client sets none.
    void observeFrames(std::unique_ptr<FrameObserver> observer);
    /// What the game announces, for anything in the client to hear.
    EventBus& getEventBus() { return eventBus_; }

    /// Tells the interface what the connected pad calls its own buttons, so
    /// the Key Bindings panel reads Square rather than PAD3. Once per pad.
    void namePadKeysForInterface();

    /// Tell the stall watchdog we are alive. Call from long synchronous work that
    /// keeps presenting frames itself (e.g. the world load) so it is not mistaken
    /// for a hung main loop.
    void beatWatchdog();

    // State management
    [[nodiscard]] AppState getState() const { return loop_ ? loop_->state() : initialState_; }
    void requestState(AppState newState);

    // Accessors
    Window* getWindow() { return window.get(); }
    /// The world map on a window of its own, for a second monitor. Null until
    /// the setting first opens it; see updateMapWindow.
    ui::MapWindow* getMapWindow() { return mapWindow_.get(); }
    rendering::Renderer* getRenderer() { return renderer.get(); }
    ui::UIManager* getUIManager() { return uiManager.get(); }
    auth::AuthHandler* getAuthHandler() { return authHandler.get(); }
    game::GameHandler* getGameHandler() { return gameHandler.get(); }
    pipeline::AssetManager* getAssetManager() { return assetManager.get(); }
    /// The interface of the character in the world, in whatever state it is;
    /// null with no character in the world.
    addons::AddonManager* getAddonManager() { return characterAddons(); }
    /// The character in the world, if any - see CharacterSession.
    CharacterSession* getCharacterSession() { return session(); }
    game::ExpansionRegistry* getExpansionRegistry() { return expansionRegistry_.get(); }

    /// What assets were found at startup. Taken once, because it walks the
    /// override tree and no screen should do that while it is being drawn.
    const pipeline::AssetInventory& getAssetInventory() const { return assetInventory_; }
    /// Whether GitHub has a newer release than this build, asked once at
    /// startup. The login screen reads it; nothing else needs to.
    const UpdateCheck& getUpdateCheck() const { return updateCheck_; }
    pipeline::DBCLayout* getDBCLayout() { return dbcLayout_.get(); }
    bool setAssetExpansionOverride(const std::string& id);
    [[nodiscard]] const std::string& getAssetExpansionOverride() const { return assetExpansionOverrideId_; }
    void reloadExpansionData();
    /// The opcode table, update fields, packet parsers and DBC layouts a
    /// protocol profile brings with it. Loaded at startup and on every
    /// expansion change, which used to be two copies.
    void loadExpansionTables(const game::ExpansionProfile& profile); // Reload DBC layouts, opcodes, etc. after expansion change

    // Singleton access
    static Application& getInstance() { return *instance; }

    // Render bounds lookup (for click targeting / selection) - delegates to EntitySpawner
    bool getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const;
    bool getRenderFootZForGuid(uint64_t guid, float& outFootZ) const;
    /// Light a game object's model while the player is pressing on it.
    ///
    /// A game object is used rather than selected, so it has no circle under
    /// it; the press lighting the model is what says the click landed on this
    /// one. Guid 0 takes the light off whatever had it.
    void setPressedGameObject(uint64_t guid);
    bool getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const;

    // Character skin composite state - delegated to AppearanceComposer
    [[nodiscard]] const std::string& getBodySkinPath() const { return appearanceComposer_ ? appearanceComposer_->getBodySkinPath() : emptyString_; }
    [[nodiscard]] const std::vector<std::string>& getUnderwearPaths() const { return appearanceComposer_ ? appearanceComposer_->getUnderwearPaths() : emptyStringVec_; }
    [[nodiscard]] uint32_t getSkinTextureSlotIndex() const { return appearanceComposer_ ? appearanceComposer_->getSkinTextureSlotIndex() : 0; }
    [[nodiscard]] uint32_t getCloakTextureSlotIndex() const { return appearanceComposer_ ? appearanceComposer_->getCloakTextureSlotIndex() : 0; }
    [[nodiscard]] uint32_t getGryphonDisplayId() const { return entitySpawner_ ? entitySpawner_->getGryphonDisplayId() : 0; }
    [[nodiscard]] uint32_t getWyvernDisplayId() const { return entitySpawner_ ? entitySpawner_->getWyvernDisplayId() : 0; }

    // Entity spawner access
    EntitySpawner* getEntitySpawner() { return entitySpawner_.get(); }

    // Appearance composer access
    AppearanceComposer* getAppearanceComposer() { return appearanceComposer_.get(); }

    // World loader access
    WorldLoader* getWorldLoader() { return worldLoader_.get(); }

    // Audio coordinator access
    audio::AudioCoordinator* getAudioCoordinator() { return audioCoordinator_.get(); }

    /// Which zone the player is in - see ZoneTracker.
    ZoneTracker* getZoneTracker() { return zoneTracker_.get(); }

    /// The local player's camera, facing and animation - see PlayerView.
    PlayerView* getPlayerView() { return playerView_.get(); }
    rendering::CameraController* getCameraController();
    rendering::AnimationController* getAnimationController();
    /// Spell effects in the world, for every caster.
    rendering::SpellVisualSystem* getSpellVisualSystem() { return spellVisualSystem_.get(); }
    /// The screen being recorded to a video file.
    ScreenRecording* getScreenRecording() { return screenRecording_.get(); }
    /// The frame statistics overlay, F1.
    rendering::PerformanceHUD* getPerformanceHUD() { return performanceHUD_.get(); }
    /// The world map; null until the world's renderers are up.
    rendering::WorldMap* getWorldMap() { return worldMap_.get(); }

    /// After the renderer has built the world's renderers - on entering a
    /// world, or a map without terrain: the camera collides with them, the
    /// spell visuals draw through them, and the world map is built.
    void onWorldRenderersReady();
    /// The map every view of the world is looking at: the renderer's, and the
    /// world map's.
    void setActiveMapName(const std::string& name);

private:
    // ---- ClientWindow ---------------------------------------------------------
    [[nodiscard]] bool shouldClose() const override;
    void requestClose() override;
    float beginFrame() override;
    void pace() override;

    // ---- ClientInput: SDL's queue, the pad, the touch controls ---------------
    void pump(float deltaTime, const FramePlan& plan, InterfaceInput& forInterface) override;

    // ---- ClientFrontEnd --------------------------------------------------------
    void updateLogin(float deltaTime, const FramePlan& plan) override;
    void prepareFrame() override;
    void updateWorld(float deltaTime) override;
    void streamScene(float deltaTime, const FramePlan& plan) override;
    void showLoadProgress(const WorldLoadStatus& status) override;
    void afterInterfaceInput() override;
    [[nodiscard]] bool canDrawInterface() const override;
    void updateScreens(float deltaTime) override;
    void drawInterface(float deltaTime, const FramePlan& plan) override;
    void keyForClient(const SDL_Event& event) override;
    void clientShortcut(ClientShortcut shortcut) override;
    void present(float deltaTime, const FramePlan& plan) override;
    [[nodiscard]] bool scenePending() const override;
    void render(const FramePlan& plan) override;
    void afterTransitions() override;
    [[nodiscard]] bool profilingStages() const override { return frameProfileEnabled_; }
    void frameFinished() override;
    void mapEntryBegun(const WorldLoader::Entry& entry) override;
    void mapRenderersReady(const WorldLoader::Entry& entry, const std::string& mapName) override;
    void placeOnInstanceFloor(const WorldLoader::Entry& entry) override;
    void mapGeometryReady(const WorldLoader::Entry& entry) override;
    void enteredWorld() override;
    void characterScreenShown() override;
    void characterLeft(bool keptConnection) override;
    void showDisconnectNotice(const std::string& notice) override;
    void showReconnectStatus(const std::string& status) override { reconnectStatus_ = status; }
    void exiting() override;

    /// The frame and the world transitions, built once initialize has made
    /// everything they drive.
    void buildClientLoop();

    /// Tell the renderer what the game knows about the world - the zone,
    /// the realm's clock and weather, the player's state - before it updates.
    /// The renderer asks the game nothing itself.
    void presentWorldToRenderer(float deltaTime);
    /// What follows the renderer's update and depends on what it worked out:
    /// sound placed where the camera is, the zone's music and ambience, and
    /// the indoor/outdoor crossing announced to the interface.
    void followRendererUpdate(float deltaTime);

    /// Opens or closes the map window to match its setting, and builds its
    /// frame. After the game's own interface frame, before the frame is
    /// submitted: the window draws into the same frame.
    void updateMapWindow(const FramePlan& plan);

    /// In-world work in the Game phase. The checkpoint and local stage labels
    /// identify a failure without obscuring runFrame's phase order.
    void updateInGame(float deltaTime, const char*& updateCheckpoint);

    /// Everything the server says about how the player moves - speeds, rooting,
    /// gravity, feather fall, water walking - handed to the camera that owns it.
    void applyServerMovementState(float deltaTime);

    /// Keep render instances on top of what the server says. A model is placed
    /// once at spawn and would otherwise stay there while its target circle
    /// follows the entity, which reads as a ring sliding off a still NPC.
    void syncRenderInstancesToEntities(float deltaTime);
    void renderFrame(const FramePlan& plan);

    /// The character in the world - see ClientLoop; null without one.
    CharacterSession* session() { return loop_ ? loop_->session() : nullptr; }
    /// The character in the world's interface, in any state; null without one.
    addons::AddonManager* characterAddons() { return loop_ ? loop_->characterAddons() : nullptr; }
    /// The same, only while it hears the world (CharacterSession::
    /// interfaceHoldsWorld): what game events, input and drawing go to.
    addons::AddonManager* worldInterface() { return loop_ ? loop_->worldInterface() : nullptr; }
    /// The current map, which the character in the world owns; null between.
    MapScope* currentMap() { return loop_ ? loop_->currentMap() : nullptr; }
    /// Records how long one named stage of a frame took.
    ///
    /// The per-stage timings only ever spoke up above 50ms, which says which
    /// stage stalled and nothing about where a frame's time normally goes. On
    /// a phone the whole budget is 33ms, so every stage was silent and the
    /// client was slow for reasons nothing reported. This keeps a running
    /// average and worst case per stage and prints the breakdown periodically.
    void noteStageTime(const char* stage, float milliseconds) override;
    void reportStageTimes();

    struct StageStat {
        double totalMs = 0.0;
        float worstMs = 0.0f;
        int frames = 0;
    };
    /// WOWEE_FRAME_PROFILE. The breakdown below is reported at info, which the
    /// log a bug report arrives with does not carry - so the one measurement
    /// that answers "where does the frame go" was never in the logs it was
    /// built for. With the flag set it is reported at warning instead.
    bool frameProfileEnabled_ = false;
    std::map<std::string, StageStat> stageStats_;
    std::chrono::steady_clock::time_point stageStatsSince_{};
    /// What assets were found at startup, so the login screen can say so
    /// rather than every screen finding out separately.
    pipeline::AssetInventory assetInventory_;
    UpdateCheck updateCheck_;


    int stageStatFrames_ = 0;

    /// WOWEE_SCREENSHOT: frames drawn before the picture is taken, so the
    /// interface has settled into what it will actually look like.
    static constexpr int kScreenshotFrame = 30;
    int screenshotFrames_ = 0;
    /// WOWEE_RECORD: a recording from start-up, then quit. See runFrame.
    int envRecordFrames_ = 0;
    std::chrono::steady_clock::time_point envRecordStart_{};

    void setupUICallbacks();
    void spawnPlayerCharacter();
    // Re-spawn the in-world player model in place after a live appearance change
    // (barber shop), so the new hair/facial hair shows without a restart.
    void refreshPlayerCharacterModel();
    void buildFactionHostilityMap(uint8_t playerRace);
    void setupTestTransport();  // Test transport boat for development

    static Application* instance;

    /// The CVars, for the game, the interface and the addons alike. Loaded
    /// first thing in initialize: the renderer reads one before it is built.
    core::CVarStore cvars_{core::getConfigRoot() + "/cvars.cfg"};
    /// What the game announces, for anything in the client to hear. Declared
    /// before everything that subscribes, so it is destroyed after them.
    core::EventBus eventBus_;
    game::GameServices gameServices_;
    /// What the emotes are, for the game, the interface and the animation.
    std::unique_ptr<game::EmoteTable> emoteTable_;

    std::unique_ptr<Window> window;
    std::unique_ptr<rendering::Renderer> renderer;
    std::unique_ptr<ui::UIManager> uiManager;
    std::unique_ptr<ui::MapWindow> mapWindow_;
    /// A failed open is not retried every frame; switching the setting off
    /// and on again tries again.
    bool mapWindowFailed_ = false;
    std::unique_ptr<auth::AuthHandler> authHandler;
    /// How the frame renews a session lost in the world, over authHandler.
    std::unique_ptr<LoginServerRenewal> sessionRenewal_;
    /// Said over the world while the client reconnects; empty otherwise.
    std::string reconnectStatus_;
    std::unique_ptr<game::GameHandler> gameHandler;
    /// The renderer's side of the transports - see game::TransportScene.
    std::unique_ptr<RendererTransportScene> transportScene_;
    // The walls other players run into between their movement reports.
    std::unique_ptr<game::MovementCollision> movementCollision_;
    std::unique_ptr<pipeline::AssetManager> assetManager;
    /// The addons on disk and which are switched on: the player's, not a
    /// character's. Each character's interface loads from it.
    std::unique_ptr<addons::AddonCatalog> addonCatalog_;
    /// What every character's interface is handed to reach the client: the
    /// settings, the map, the audio. Built once at startup; the closures name
    /// process-wide objects or ask for the current session.
    addons::LuaServices luaServices_;
    /// The run's Lua errors, across every character's interface and reload.
    std::unique_ptr<addons::LuaErrorReport> luaErrorReport_;
    /// Draws the widget tree addons build through CreateFrame/CreateTexture.
    /// Holds the texture cache for Interface\ art, so it lives as long as the app.
    ui::WidgetRenderer widgetRenderer_;
    /// The live head-and-shoulders view the interface's portrait draws.
    ui::UnitPortrait unitPortrait_;
    /// Where the portrait's widget was found last time. Ids are stable, so
    /// this saves a scan of every widget by name on every frame; the name is
    /// still checked, because reloading the interface rebuilds the tree and
    /// the id could then belong to something else.
    uint32_t portraitWidgetId_ = 0;
    /// The FrameXML frame the real minimap is drawn into, when the original
    /// interface owns it. Looked up by name and remembered, the same as the
    /// portrait.
    uint32_t minimapWidgetId_ = 0;
    /// The FrameXML frame the world map is drawn into, when the original
    /// interface owns it. WorldMapDetailFrame rather than WorldMapFrame: the
    /// first is the map area, the second is the panel around it.
    uint32_t worldMapWidgetId_ = 0;
    /// The target's face, on the same terms as the player's. A third offscreen
    /// pass because all three are on screen at once and each holds a different
    /// model - the alternative is reloading a model per frame, which is what a
    /// shared view would amount to.
    ui::UnitPortrait targetPortrait_;
    /// And the focus, which is deliberate enough to be worth its own pass.
    ui::UnitPortrait focusPortrait_;
    /// The dressing room's figure, and its frame. The player plus whatever
    /// has been tried on, which is the paperdoll with an overlay.
    ui::UnitPortrait dressUpModel_;
    uint32_t dressUpWidgetId_ = 0;
    /// The auction house's own dressing room, which is a second frame with a
    /// second try-on list rather than the same one reused.
    ui::UnitPortrait auctionDressUpModel_;
    uint32_t auctionDressUpWidgetId_ = 0;
    /// The pet tab's figure. A creature by display id, like its portrait.
    ui::UnitPortrait petModel_;
    uint32_t petModelWidgetId_ = 0;
    /// The stable's preview, which shows whichever slot the window selected
    /// rather than the pet that is out.
    ui::UnitPortrait stableModel_;
    uint32_t stableModelWidgetId_ = 0;
    /// The mount or critter the companion tab has selected.
    ui::UnitPortrait companionModel_;
    uint32_t companionModelWidgetId_ = 0;
    /// The inspect window's figure, and the frame it is drawn into. Whole
    /// body at the paperdoll's size, because that is what it is: someone
    /// else's paperdoll.
    ui::UnitPortrait inspectModel_;
    uint32_t inspectModelWidgetId_ = 0;
    /// And whoever this client is dealing with - the face in the gossip,
    /// quest, merchant, flight master and trade panels. One view for all of
    /// them because only one such window is open at a time.
    ui::UnitPortrait npcPortrait_;
    /// And the four party members. Affordable now that a portrait is sized for
    /// the circle it is drawn into rather than for the paperdoll: at 160x200 a
    /// party face is a sixteenth of the pixels the paperdoll's target is, and
    /// four of them together cost a quarter of one of it.
    std::array<ui::UnitPortrait, 4> partyPortraits_;
    /// And the pet's, which is always a creature and so always has one.
    ui::UnitPortrait petPortrait_;

    /// The paperdoll's model view, and the frame it is drawn into. A second
    /// offscreen pass rather than a shared one: the portrait shows the face
    /// and this shows the whole figure, and they are on screen together.
    ui::UnitPortrait paperdollModel_;
    /// The facing already applied to the paperdoll, so only the change since
    /// last frame is turned.
    float paperdollFacing_ = 0.0f;
    uint32_t paperdollWidgetId_ = 0;
    std::unique_ptr<game::ExpansionRegistry> expansionRegistry_;
    // Empty means assets follow the active protocol profile. "legacy" selects
    // the root WOW_DATA_PATH manifest; otherwise this is an expansion id.
    std::string assetExpansionOverrideId_;
    std::unique_ptr<pipeline::DBCLayout> dbcLayout_;
    std::unique_ptr<EntitySpawner> entitySpawner_;
    std::unique_ptr<AppearanceComposer> appearanceComposer_;
    std::unique_ptr<WorldLoader> worldLoader_;
    std::unique_ptr<rendering::LoadingScreen> loadingScreen_;
    std::unique_ptr<audio::AudioCoordinator> audioCoordinator_;
    std::unique_ptr<ZoneTracker> zoneTracker_;
    std::unique_ptr<rendering::SpellVisualSystem> spellVisualSystem_;
    std::unique_ptr<PlayerView> playerView_;
    std::unique_ptr<ScreenRecording> screenRecording_;
    std::unique_ptr<rendering::PerformanceHUD> performanceHUD_;
    /// The process's memory, threads and caches over time: the HUD's graphs
    /// and the resource log line. Built when the loop starts.
    std::unique_ptr<ResourceMonitor> resourceMonitor_;
    std::unique_ptr<rendering::WorldMap> worldMap_;
    bool spellVisualsAttached_ = false;

    // Callback handlers (extracted from setupUICallbacks)
    std::unique_ptr<NPCInteractionCallbackHandler> npcInteractionCallbacks_;
    std::unique_ptr<AudioCallbackHandler> audioCallbacks_;
    std::unique_ptr<EntitySpawnCallbackHandler> entitySpawnCallbacks_;
    std::unique_ptr<AnimationCallbackHandler> animationCallbacks_;
    std::unique_ptr<TransportCallbackHandler> transportCallbacks_;
    std::unique_ptr<WorldEntryCallbackHandler> worldEntryCallbacks_;
    std::unique_ptr<UIScreenCallbackHandler> uiScreenCallbacks_;

    // Realm-announced patch archives, fetched over the login connection.
    // Declared after authHandler so it is destroyed first: its transport
    // calls into the auth handler.
    std::unique_ptr<RealmPatchSync> realmPatchSync_;
    std::string connectionStatus_;  // what the character screen was last told
    void startRealmPatchSync(const std::vector<game::EzWowPatch>& patches);
    void updateConnectionAndPatches(float deltaTime, const FramePlan& plan);

    // The archives of the realm connected to, read in place above the
    // extracted data once every one of them is on disk and checked. Each set
    // belongs to one realm - "<login host>:<port>/<realm address>" - and is
    // taken down when the player connects to another.
    struct RealmPatchSet {
        std::string realm;
        std::vector<std::filesystem::path> archives;
    };
    RealmPatchSet mountedRealmPatches_;
    // Set when a sync starts, consumed when it ends: the set it was for.
    std::optional<RealmPatchSet> realmPatchesToMount_;
    // Opened on a worker - reading two dozen archives' file lists is a fifth
    // of a second, more from a cold disk - and put in place on this thread
    // when no world is in use.
    std::future<std::shared_ptr<const pipeline::ArchiveLayer>> realmPatchesOpening_;
    RealmPatchSet realmPatchesBeingOpened_;
    std::shared_ptr<const pipeline::ArchiveLayer> realmPatchLayerReady_;
    [[nodiscard]] std::string currentRealmPatchKey() const;
    void updateRealmPatchMount();
    void applyRealmPatchLayer(std::shared_ptr<const pipeline::ArchiveLayer> layer, RealmPatchSet set);

    // Beat by the main loop each iteration; the watchdog treats a long silence as a
    // hang. Long but healthy work that renders its own frames (world load) must beat
    // it too, or the watchdog mistakes the load for a hang.
    std::atomic<int64_t> watchdogHeartbeatMs_{0};
    std::atomic<bool> watchdogRequestRelease_{false};

    /// The state the client starts in: the login screen, or the asset
    /// builder when nothing is installed. The loop holds it from then on.
    AppState initialState_ = AppState::AUTHENTICATION;
    bool running = false;
    Phase currentPhase_ = Phase::Idle;
    FramePacer pacer_;
    bool spawnSnapToGround = true;
    float lastFrameTime = 0.0f;

    // Player character info (for model spawning)
    game::Race playerRace_ = game::Race::HUMAN;
    game::Gender playerGender_ = game::Gender::MALE;

    // Static empty values for null-safe delegation
    static inline const std::string emptyString_;
    static inline const std::vector<std::string> emptyStringVec_;

    // The facing send rate limit, the idle yawn and the auto-attack, swim and
    // indoor edges are the character's: CharacterSession::PlayerSync. The
    // transport riding locks and the pending deck floor are the map's:
    // MapScope::TransportRide.

    // Quest marker billboard sprites (above NPCs)
    void loadQuestMarkerModels();  // Now loads BLP textures
    void updateQuestMarkers();     // Updates billboard positions
    void updateLootSparkles();     // The glitter over corpses with loot
    void updateCorpseBones();      // Players' bones, as their race's skeleton

    /// The frame and the world transitions, and with them the character in
    /// the world. Last, so it goes before everything it borrows; shutdown()
    /// ends the character explicitly before any of those.
    std::unique_ptr<ClientLoop> loop_;
};

} // namespace core
} // namespace wowee

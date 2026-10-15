#pragma once

#include "addons/interface_state.hpp"
#include "addons/lua_services.hpp"
#include "core/character_identity.hpp"
#include "core/event_bus.hpp"
#include "core/frame_plan.hpp"
#include "core/map_scope.hpp"
#include "core/world_loader.hpp"

#include <cstdint>
#include <memory>
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
class PlayerView;
class SpellPresentation;

/// Everything the client holds for one character in the world.
///
/// Created when a character enters the world - the first map load after the
/// server verified the login - and destroyed at logout, disconnect, return to
/// character select and exit. Construction builds it and the destructor tears
/// it down; nothing in it is cleared by hand, so leaving by any route leaves
/// nothing of this character for the next one. Created and destroyed only in
/// the Transitions phase of the client frame, like the MapScope inside it.
///
/// What it owns, in declaration order (released in reverse):
///
///   InterfaceState     the interface's own per-character state: collapsed
///                      quest groups, LFG ticks, auction sort, the cursor, ...
///                      Outlives the Lua state that reads it, and a /reload.
///   SpellPresentation  how spells look and sound; its bus subscriptions and
///                      the restoration/aura timers end with the character.
///   PlayerSync         the client's per-character frame state (facing sends,
///                      the auto-attack and swim edges, the indoor crossing).
///   AddonManager       the Lua state, FrameXML and the addons built in it.
///                      Its destructor writes the saved variables under this
///                      character's name, then closes the state.
///   MapScope           the current map. A map change replaces only this; the
///                      session ends it first, before anything else goes.
///
/// Process-wide state stays outside: the window, settings, key bindings, the
/// addon catalog and enabled choices (addons.cfg), the chat layout and bar
/// toggles in interface_state.cfg, renderer caches, the camera rig and its
/// settings (PlayerView), and the GameHandler, which is the session with the
/// server rather than with a character.
class CharacterSession {
public:
    /// See core/character_identity.hpp, which also says where it comes from.
    using Identity = CharacterIdentity;

    /// How much of an interface this character gets.
    enum class Interface {
        /// FrameXML and the enabled addons, loaded at the first world entry.
        Full,
        /// A Lua state that hears game events and OnUpdate from the start and
        /// loads nothing: the offline loading fixture, whose character has no
        /// unit fields for FrameXML's login handlers to read.
        EventsOnly,
        /// No Lua state at all: an offline scene with no character.
        None,
    };

    struct Services {
        Phase& phase;
        EventBus& events;
        game::GameHandler& game;
        /// Null in a client that draws nothing.
        rendering::Renderer* renderer = nullptr;
        EntitySpawner& spawner;
        rendering::SpellVisualSystem* spellVisuals = nullptr;
        audio::AudioCoordinator* audio = nullptr;
        pipeline::AssetManager& assets;
        /// Null when the addon system could not start.
        addons::AddonCatalog* addonCatalog = nullptr;
        addons::LuaServices luaServices;
        /// Process-wide presenters that carry something of the character and
        /// are told when it leaves (see the destructor).
        PlayerView& playerView;
        AppearanceComposer* appearance = nullptr;
        AnimationCallbackHandler* animationCallbacks = nullptr;
    };

    CharacterSession(const Services& services, Identity identity, Interface kind);
    ~CharacterSession();

    CharacterSession(const CharacterSession&) = delete;
    CharacterSession& operator=(const CharacterSession&) = delete;

    [[nodiscard]] const Identity& identity() const { return identity_; }

    // ---- The map ----------------------------------------------------------

    /// The map the character is on; null between maps (none entered yet).
    [[nodiscard]] MapScope* map() { return map_.get(); }
    [[nodiscard]] const MapScope* map() const { return map_.get(); }
    /// Entering a map: ends the current one, then builds the next and starts
    /// its load.
    MapScope& enterMap(const MapScope::Services& services, WorldLoader& loader,
                       WorldLoader::Entry entry);
    /// A scene with nothing to load (the offline probes).
    MapScope& enterScene(const MapScope::Services& services, uint32_t mapId);

    // ---- The interface ----------------------------------------------------

    /// This character's interface, or null for Interface::None or when the
    /// addon system is unavailable.
    [[nodiscard]] addons::AddonManager* addonManager() { return interface_.addons.get(); }
    [[nodiscard]] addons::InterfaceState& interfaceState() { return interfaceState_; }

    /// Whether the interface hears the world: game events, OnUpdate, input and
    /// drawing. True from the first world entry (from construction for
    /// EventsOnly) until it has been told the character is leaving.
    [[nodiscard]] bool interfaceHoldsWorld() const;
    /// Whether the interface has been told the character is leaving. Such a
    /// session only waits to be ended.
    [[nodiscard]] bool interfaceHasLeft() const { return interfaceLeft_; }

    /// A map finished loading: the interface hears about it at the next
    /// Interface phase (enterWorldInterface).
    void noteMapReady() { worldEntryPending_ = true; }
    /// In the Interface phase: the first entry loads the interface and sends
    /// the login events, every later one PLAYER_ENTERING_WORLD.
    void enterWorldInterface();
    /// In the Interface phase, while the world still stands: tell the
    /// interface the character is leaving. Once; afterwards it hears nothing
    /// and the session is ready to go in Transitions.
    void leaveWorldInterface();
    /// ReloadUI or /reload: the Lua state is built again, between frames.
    void requestInterfaceReload() { reloadPending_ = true; }
    /// In the Interface phase: perform a requested reload. The character, its
    /// InterfaceState and its map stay; the Lua state, FrameXML and the addons
    /// are built anew, after the old state wrote its saved variables.
    void reloadInterfaceIfRequested();

    // ---- Presenters and per-character frame state --------------------------

    [[nodiscard]] SpellPresentation* spellPresentation() { return spellPresentation_.get(); }

    /// The client's own per-character frame state, which Application's frame
    /// reads and writes while the character is in the world.
    struct PlayerSync {
        /// Rate limit and last value of MSG_MOVE_SET_FACING.
        float facingSendCooldown = 0.0f;
        float lastSentCanonicalYaw = 1000.0f;   // sentinel: the first frame sends
        bool idleYawned = false;
        bool wasAutoAttacking = false;
        /// Whether the player was swimming last frame, so weapons are put away
        /// once on entering the water rather than every frame in it.
        bool wasSwimmingForSheath = false;
        /// Whether the camera was inside a WMO last frame, for announcing the
        /// crossing once.
        bool cameraIndoors = false;
    };
    [[nodiscard]] PlayerSync& playerSync() { return playerSync_; }

    /// Which pad's button names were last written into this interface's Lua
    /// state, as a core::Gamepad::Kind value (an int, to keep SDL out of this
    /// header); -1 for none yet. Belongs to the Lua state, so a /reload starts
    /// it again.
    [[nodiscard]] int& padKeyNamesWritten() { return interface_.padKeyNames; }

private:
    /// One build of the interface: what a /reload replaces whole.
    struct BuiltInterface {
        std::unique_ptr<addons::AddonManager> addons;
        int padKeyNames = -1;
    };
    BuiltInterface buildInterface();
    void loadInterface();

    Services services_;
    const Identity identity_;
    const Interface interfaceKind_;

    // Declaration order is the reverse of release. The map goes first, the
    // interface (which writes the saved variables while the Lua state still
    // holds them) second, the presenters after, and the state the bindings
    // read last of all, since the Lua state reaches it until it closes.
    addons::InterfaceState interfaceState_;
    std::unique_ptr<SpellPresentation> spellPresentation_;
    PlayerSync playerSync_;
    BuiltInterface interface_;
    bool interfaceEntered_ = false;
    bool interfaceLeft_ = false;
    bool worldEntryPending_ = false;
    bool reloadPending_ = false;
    std::unique_ptr<MapScope> map_;
};

} // namespace core
} // namespace wowee

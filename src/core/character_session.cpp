#include "core/character_session.hpp"

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/animation_callback_handler.hpp"
#include "core/appearance_composer.hpp"
#include "core/frame_phase.hpp"
#include "core/logger.hpp"
#include "core/player_view.hpp"
#include "core/spell_presentation.hpp"
#include "rendering/camera_controller.hpp"
#include "game/account_data.hpp"
#include "game/game_events.hpp"
#include "game/game_handler.hpp"

namespace wowee::core {

CharacterSession::CharacterSession(const Services& services, Identity identity, Interface kind)
    : services_(services), identity_(std::move(identity)), interfaceKind_(kind) {
    LOG_INFO("Character session started: guid=0x", std::hex, identity_.guid, std::dec,
             " name='", identity_.name, "'");
    if (services_.renderer && services_.assets) {
        spellPresentation_ = std::make_unique<SpellPresentation>(
            services_.events, services_.game, *services_.renderer, services_.spawner,
            services_.spellVisuals, services_.audio, *services_.assets);
    } else {
        LOG_WARNING("Character session '", identity_.name, "': no ",
                    services_.renderer ? "game data" : "renderer",
                    ", so spells are neither shown nor heard");
    }
    subscriptions_.push_back(services_.events.subscribe<game::events::AccountDataLoaded>(
        [this](const game::events::AccountDataLoaded& e) {
            const auto type = static_cast<game::AccountDataType>(e.type);
            if (type == game::AccountDataType::GlobalBindings ||
                type == game::AccountDataType::CharacterBindings) {
                keyBindingsStale_ = true;
            }
        }));
    // The character is leaving and the server is taking the world apart: from
    // here on the world is not drawn, while it stands until the logout
    // completes and this session ends.
    subscriptions_.push_back(services_.events.subscribe<game::events::LogoutCommitted>(
        [this](const game::events::LogoutCommitted& e) {
            if (logoutCommitted_) return;
            logoutCommitted_ = e.exiting;
            LOG_INFO("Character '", identity_.name, "' is leaving: the world is no longer drawn");
        }));
    if (interfaceKind_ == Interface::None) return;
    if (!services_.addonCatalog) {
        LOG_WARNING("Character session '", identity_.name, "': the addon system did "
                    "not start, so this character plays without FrameXML or addons");
        return;
    }
    if (identity_.name.empty() && interfaceKind_ == Interface::Full) {
        LOG_WARNING("Character session guid=0x", std::hex, identity_.guid, std::dec,
                    ": no character name, so per-character saved variables are "
                    "neither read nor written this session");
    }
    // Read again for every character: an addon dropped in or switched on at
    // character select is loaded by the next character in.
    services_.addonCatalog->rescan();
    interface_ = buildInterface();
    // An interface with nothing to load hears the world from the start.
    if (interfaceKind_ == Interface::EventsOnly) interfaceEntered_ = true;
}

CharacterSession::~CharacterSession() {
    LOG_INFO("Character session ending: guid=0x", std::hex, identity_.guid, std::dec,
             " name='", identity_.name, "'");
    // The map first: its work is cancelled and joined, and its instances
    // released, before anything it could still reach goes.
    map_.reset();
    // Then the interface, inside an Interface scope because closing a Lua
    // state can run Lua (a __gc on a proxy). Its destructor writes the saved
    // variables under this character's name before the state closes.
    {
        ScopedPhase interfaceTeardown(services_.phase, Phase::Interface);
        interface_ = {};
    }
    // What the process-wide presenters carried of this character: the combat
    // stance, the swings and spells in flight, a charge, the sheathed weapons.
    // The next character starts from none of it.
    services_.playerView.resetCombatVisualState();
    if (services_.appearance) services_.appearance->setWeaponsSheathed(false);
    if (services_.animationCallbacks) services_.animationCallbacks->resetChargeState();
    // The rest - the frame state, the spell presentation and its subscriptions,
    // the interface state - goes with the members, in reverse of declaration.
}

MapScope& CharacterSession::enterMap(const MapScope::Services& services, WorldLoader& loader,
                                     WorldLoader::Entry entry) {
    // The previous map - finished, or a load this entry supersedes, even on
    // the same map - ends before the next scope binds the spawner.
    if (map_) {
        LOG_INFO("Map change: ending map ", map_->mapId(), " before loading map ", entry.mapId);
        map_.reset();
    }
    map_ = std::make_unique<MapScope>(services, loader, entry);
    return *map_;
}

MapScope& CharacterSession::enterScene(const MapScope::Services& services, uint32_t mapId) {
    map_.reset();
    map_ = std::make_unique<MapScope>(services, mapId);
    return *map_;
}

std::unique_ptr<MapScope> CharacterSession::releaseMap() {
    return std::move(map_);
}

void CharacterSession::adoptMap(std::unique_ptr<MapScope> map) {
    if (map_) {
        LOG_ERROR("Character session '", identity_.name, "' handed map ",
                  map ? static_cast<int>(map->mapId()) : -1, " while it stands on map ",
                  map_->mapId(), "; its own map ends first");
        map_.reset();
    }
    map_ = std::move(map);
}

CharacterSession::BuiltInterface CharacterSession::buildInterface() {
    // Lua starts in an Interface scope, as the client's startup does.
    ScopedPhase interfaceInit(services_.phase, Phase::Interface);
    BuiltInterface built;
    built.addons = std::make_unique<addons::AddonManager>(
        *services_.addonCatalog, interfaceState_, &services_.game, services_.luaServices,
        identity_.name);
    if (auto* engine = built.addons->getLuaEngine()) {
        // Not addUIError: that reports by firing an event, which runs script,
        // which is how a script error came to report itself.
        engine->setLuaErrorCallback([game = &services_.game](const std::string& err) {
            game->addScriptError(err);
        });
        // The game-menu button opens this client's settings panel.
        engine->setOpenSettingsCallback([open = services_.luaServices.openSettings] {
            if (open) open({});
        });
    }
    return built;
}

bool CharacterSession::interfaceHoldsWorld() const {
    return interface_.addons && interfaceEntered_ && !interfaceLeft_;
}

float CharacterSession::noteTimeSinceLogoutCommitted(float deltaTime) {
    if (!logoutCommitted_) return 0.0f;
    sinceLogoutCommitted_ += deltaTime;
    return sinceLogoutCommitted_;
}

void CharacterSession::loadInterface() {
    auto& addons = *interface_.addons;
    addons.loadAllAddons();
    // The bindings in force before anything asks for them, as the original
    // client loads them: nothing in the interface calls LoadBindings at
    // startup, so without this the saved set stayed in the account data and
    // every key answered to its default.
    if (auto* engine = addons.getLuaEngine()) engine->reloadKeyBindings();
    keyBindingsStale_ = false;
    // Built: from here it hears the world, the login burst included.
    interfaceEntered_ = true;
    addons.fireEvent("VARIABLES_LOADED");
    // After VARIABLES_LOADED, which is when the chat window settings are
    // available to be read. Every chat frame answers this by applying its
    // saved font, colour, size and docked state; without it they keep the
    // defaults they were built with however much was saved.
    addons.fireEvent("UPDATE_CHAT_WINDOWS");
    addons.fireEvent("PLAYER_LOGIN");
    addons.fireEvent("PLAYER_ENTERING_WORLD");
}

void CharacterSession::enterWorldInterface() {
    if (!requirePhase(services_.phase, Phase::Interface, "interface world entry")) return;
    if (!worldEntryPending_) return;
    worldEntryPending_ = false;
    if (!interface_.addons || interfaceLeft_) return;
    if (!interfaceEntered_) {
        // The first world entry of this character: the interface is built.
        loadInterface();
    } else {
        // Every later one - a teleport, an instance - is announced.
        interface_.addons->fireEvent("PLAYER_ENTERING_WORLD");
    }
}

void CharacterSession::leaveWorldInterface() {
    if (!requirePhase(services_.phase, Phase::Interface, "interface teardown")) return;
    if (interfaceLeft_) return;
    // Said while the world still stands, which is why it is here and not in
    // the destructor: handlers read the player and the map as they leave.
    if (interfaceHoldsWorld()) interface_.addons->fireEvent("PLAYER_LEAVING_WORLD");
    interfaceLeft_ = true;
    // From here no key's release reaches the bindings, so what they hold -
    // a walk begun on W - is let go now rather than never.
    releaseBindingMovement();
}

void CharacterSession::releaseBindingMovement() {
    if (auto* camera = services_.playerView.getCameraController()) {
        camera->releaseMovement(rendering::MovementSource::Bindings);
    }
}

void CharacterSession::applyChangedKeyBindings() {
    if (!keyBindingsStale_ || !interfaceHoldsWorld()) return;
    if (!requirePhase(services_.phase, Phase::Interface, "key binding reload")) return;
    keyBindingsStale_ = false;
    if (auto* engine = interface_.addons->getLuaEngine()) engine->reloadKeyBindings();
}

void CharacterSession::reloadInterfaceIfRequested() {
    if (!reloadPending_) return;
    reloadPending_ = false;
    if (!requirePhase(services_.phase, Phase::Interface, "interface reload")) return;
    if (!interface_.addons) {
        LOG_WARNING("Interface reload asked for '", identity_.name,
                    "', which has no interface to reload");
        return;
    }
    if (interfaceLeft_) {
        LOG_WARNING("Interface reload asked for '", identity_.name,
                    "' while leaving the world; not reloaded");
        return;
    }
    LOG_INFO("Reloading the interface of '", identity_.name, "'");
    // The old state writes its saved variables and closes before the new one
    // reads them back; the InterfaceState it read stays, as the character does.
    // The keys it held go with it: their releases would reach a state that
    // never saw the presses, so what they hold is let go here.
    interface_ = {};
    releaseBindingMovement();
    interface_ = buildInterface();
    if (!interfaceEntered_ || interfaceKind_ != Interface::Full) return;
    // The same events a world entry sends, so an interface reloaded from a
    // popup comes up in the same state as one reloaded from chat. Silent through
    // the burst: the unit frames initialize their dropdowns on these events and
    // every initializer ends in PlaySound("igMainMenuOpen"). See
    // LuaEngine::setUiSoundsSuppressed.
    addons::LuaEngine::setUiSoundsSuppressed(true);
    loadInterface();
    addons::LuaEngine::setUiSoundsSuppressed(false);
}

} // namespace wowee::core

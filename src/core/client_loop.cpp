#include "core/client_loop.hpp"

#include "addons/addon_manager.hpp"
#include "core/character_identity.hpp"
#include "core/coordinates.hpp"
#include "core/frame_observer.hpp"
#include "core/frame_phase.hpp"
#include "core/logger.hpp"
#include "core/map_scope.hpp"
#include "core/player_view.hpp"
#include "core/profiler.hpp"
#include "core/spell_presentation.hpp"
#include "core/world_entry_callback_handler.hpp"
#include "game/game_handler.hpp"
#include "game/movement_handler.hpp"
#include "rendering/camera_controller.hpp"

#include <algorithm>
#include <exception>

namespace wowee::core {

namespace {

/// After a reconnect onto the same map, how long the server has to show again
/// what the client held: a login's objects in view arrive within a second of
/// SMSG_LOGIN_VERIFY_WORLD, in the first map update after the character is
/// added. What has not come by then has gone while the client was away.
constexpr float kHeldReleaseSeconds = 3.0f;

} // namespace

ClientLoop::ClientLoop(Services services, AppState initialState)
    : services_(std::move(services)), state_(initialState), transitions_(services_.events) {
    // The game's FrameXML channel goes to the interface of the character in
    // the world, while it hears it: game events, and the commands a key
    // binding runs on a frame that replaced one of this client's windows.
    services_.game.setAddonEventCallback([this](const std::string& event,
                                                const std::vector<std::string>& args) {
        if (auto* addons = worldInterface()) addons->fireEvent(event, args);
    });
    services_.game.setInterfaceCommandCallback([this](const std::string& lua) {
        if (auto* addons = worldInterface()) addons->runInterfaceCommand(lua);
    });
}

ClientLoop::~ClientLoop() {
    {
        ScopedPhase ending(services_.phase, Phase::Transitions);
        session_.reset();
    }
    services_.game.setAddonEventCallback(nullptr);
    services_.game.setInterfaceCommandCallback(nullptr);
}

void ClientLoop::setObserver(std::unique_ptr<FrameObserver> observer) {
    observer_ = std::move(observer);
}

void ClientLoop::loopEnded() {
    if (observer_) observer_->loopEnded();
}

FramePlan ClientLoop::framePlan() const {
    FramePlan plan = framePlanFor(state_);
    // A committed logout: the world stands until the server completes it,
    // which it does after taking every object in view away over several
    // frames. The screen being left for is drawn instead of that.
    if (plan.world && session_ && session_->worldHidden()) plan.view = FrameView::LeavingWorld;
    return plan;
}

void ClientLoop::watchCommittedLogout(float deltaTime) {
    if (!session_ || !session_->worldHidden() || transitions_.leaving()) return;
    const float waited = session_->noteTimeSinceLogoutCommitted(deltaTime);
    if (waited < kLogoutCompleteTimeoutSeconds) return;
    const bool exits = session_->logoutExits();
    LOG_ERROR("The server has not completed a logout committed ", waited, " s ago; leaving the "
              "world without it, ", exits ? "closing the game" : "for the login screen");
    transitions_.leave(exits ? WorldTransitionQueue::Destination::Exit
                             : WorldTransitionQueue::Destination::Login);
}

void ClientLoop::runFrame() {
    const FramePlan plan = framePlan();
    ScopedPhase frameScope(services_.phase, Phase::Idle);
    try {
        const float dt = services_.window.beginFrame();
        FrameMark;
        beginPhase(Phase::Input);
        if (observer_) observer_->frameBegan();
        services_.input.pump(dt, plan, interfaceInput_);
        if (services_.window.shouldClose()) return;
        beginPhase(Phase::Network);     dispatchNetwork(dt, plan);
        beginPhase(Phase::Game);        services_.frontEnd.prepareFrame();
        if (plan.game) updateGame(dt, plan);
        watchCommittedLogout(dt);
        beginPhase(Phase::Events);      services_.events.dispatchPosted();
        beginPhase(Phase::Streaming);   updateStreaming(dt, plan);
        beginPhase(Phase::Interface);   updateInterface(dt, plan);
        if (services_.window.shouldClose()) return;
        beginPhase(Phase::Present);     syncPresentation(dt, plan);
        beginPhase(Phase::Render);      services_.frontEnd.render(plan);
        beginPhase(Phase::Transitions); processWorldTransitions();
        services_.frontEnd.afterTransitions();
        beginPhase(Phase::Pace);        services_.window.pace();
        beginPhase(Phase::Idle);
        services_.frontEnd.frameFinished();
    } catch (const std::exception& e) {
        LOG_ERROR("Frame failed in phase ", phaseName(services_.phase),
                  " (state=", static_cast<int>(state_), "): ", e.what());
        throw;
    }
}

void ClientLoop::beginPhase(Phase phase) {
    const auto now = std::chrono::steady_clock::now();
    if (services_.frontEnd.profilingStages() && services_.phase != Phase::Idle) {
        services_.frontEnd.noteStageTime(phaseName(services_.phase),
            std::chrono::duration<float, std::milli>(now - phaseStartedAt_).count());
    }
    services_.phase = phase;
    phaseStartedAt_ = now;
}

bool ClientLoop::requireTransitions(const char* operation) const {
    return requirePhase(services_.phase, Phase::Transitions, operation);
}

MapScope::Services ClientLoop::mapScopeServices() {
    return MapScope::Services{*services_.spawner, services_.renderer, services_.game,
                              services_.playerView};
}

CharacterSession::Services ClientLoop::characterSessionServices() {
    return CharacterSession::Services{
        .phase = services_.phase,
        .events = services_.events,
        .game = services_.game,
        .renderer = services_.renderer,
        .spawner = *services_.spawner,
        .spellVisuals = services_.spellVisuals,
        .audio = services_.audio,
        .assets = services_.assets,
        .addonCatalog = services_.addonCatalog,
        .luaServices = services_.luaServices,
        .playerView = services_.playerView,
        .appearance = services_.appearance,
        .animationCallbacks = services_.animationCallbacks,
    };
}

addons::AddonManager* ClientLoop::characterAddons() {
    return session_ ? session_->addonManager() : nullptr;
}

addons::AddonManager* ClientLoop::worldInterface() {
    return session_ && session_->interfaceHoldsWorld() ? session_->addonManager() : nullptr;
}

void ClientLoop::requestState(AppState state) {
    requestedState_ = state;
}

void ClientLoop::setState(AppState newState) {
    if (!requireTransitions("application state change")) {
        requestState(newState);
        return;
    }
    if (state_ == newState) {
        return;
    }

    LOG_INFO("State transition: ", static_cast<int>(state_), " -> ", static_cast<int>(newState));
    state_ = newState;

    switch (newState) {
        case AppState::CHARACTER_SELECTION:
            services_.frontEnd.characterScreenShown();
            // The character screen has no character in the world. Leaving the
            // world went through performLogoutToLogin, which ended the
            // character's session and with it the map and the interface; this
            // is the backstop for any other way here.
            if (session_) {
                LOG_WARNING("Character screen reached with '", session_->identity().name,
                            "' still in the world (map ",
                            session_->map() ? static_cast<int>(session_->map()->mapId()) : -1,
                            "); ending that session now");
                session_.reset();
            }
            if (services_.loader) services_.loader->discardPendingEntry();
            break;
        case AppState::IN_GAME:
            // The camera's movement reaches the game from here on.
            services_.playerView.connectMovement(services_.game);
            services_.frontEnd.enteredWorld();
            break;
        default:
            // Nothing to enter: LOADING_WORLD is begun by beginWorldLoading,
            // and the login, realm, creation, disconnected and first-run
            // screens build what they show themselves.
            break;
    }
}

void ClientLoop::dispatchNetwork(float deltaTime, const FramePlan& plan) {
    services_.frontEnd.updateLogin(deltaTime, plan);
    if (!plan.worldNetwork) return;

    const auto start = std::chrono::steady_clock::now();
    services_.game.updateNetwork(deltaTime);
    networkUpdateMs_ = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

void ClientLoop::updateGame(float deltaTime, const FramePlan& plan) {
    if (reconnect_) updateReconnect(deltaTime);
    if (heldReleaseIn_ >= 0.0f) {
        heldReleaseIn_ -= deltaTime;
        if (heldReleaseIn_ < 0.0f && services_.spawner) {
            // The login's first burst is in: what it did not show is gone.
            const size_t released = services_.spawner->releaseHeld();
            LOG_INFO("Reconnect: ", released, " entities the server did not show again were removed");
        }
    }
    if (plan.worldNetwork) {
        const auto start = std::chrono::steady_clock::now();
        services_.game.updateGame(deltaTime);
        if (plan.world || plan.loading) {
            if (auto* spells = session_ ? session_->spellPresentation() : nullptr)
                spells->update(deltaTime);
        }
        if (plan.world) {
            const float ms = networkUpdateMs_ + std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            services_.frontEnd.noteStageTime("gameHandler->update", ms);
            if (ms > 50.0f) LOG_WARNING("SLOW update stage 'gameHandler->update': ", ms, "ms");
        }
    }
    if (plan.world) services_.frontEnd.updateWorld(deltaTime);
}

void ClientLoop::updateStreaming(float deltaTime, const FramePlan& plan) {
    services_.frontEnd.streamScene(deltaTime, plan);
    if (!services_.loader) return;
    const auto& status = services_.loader->update(deltaTime);
    if (plan.loading) {
        services_.frontEnd.showLoadProgress(status);
        if (status.result == WorldLoadResult::Failed && !transitions_.leaving()) {
            disconnectNotice_ = status.reason;
            transitions_.leave(WorldTransitionQueue::Destination::Login);
        }
    }
    if (observer_) observer_->streamingDone(plan);
}

bool ClientLoop::worldEndsThisFrame(const FramePlan& plan) const {
    return worldEnding() || reentryPending() ||
           (requestedState_ && *requestedState_ != AppState::IN_GAME &&
            *requestedState_ != AppState::LOADING_WORLD && (plan.world || plan.loading));
}

bool ClientLoop::worldEnding() const {
    const auto leave = transitions_.queuedLeave();
    if (!leave) return false;
    if (leave->destination != WorldTransitionQueue::Destination::Reconnect) return true;
    return !reconnect_ && !keepsWorldThroughReconnect(leave->characterGuid);
}

bool ClientLoop::reentryPending() const {
    return reconnect_ && !transitions_.leaving() && transitions_.hasWorldChange();
}

bool ClientLoop::keepsWorldThroughReconnect(uint64_t characterGuid) const {
    return services_.renewal && services_.spawner && characterGuid != 0 && session_ &&
           session_->map() && !session_->interfaceHasLeft() &&
           (state_ == AppState::IN_GAME || state_ == AppState::LOADING_WORLD);
}

void ClientLoop::beginReconnecting(uint64_t characterGuid) {
    if (!requireTransitions("begin reconnecting")) return;
    LOG_WARNING("World connection lost on map ", currentMap()->mapId(),
                "; the character stays in the world while the client reconnects");
    // What is drawn stays drawn until the server says otherwise.
    services_.spawner->holdPresented();
    // The connection, Warden and the game's world state go, as at a logout.
    services_.game.disconnect();
    reconnect_ = std::make_unique<WorldReconnect>(services_.game, *services_.renewal, characterGuid);
}

void ClientLoop::updateReconnect(float deltaTime) {
    if (reconnect_->update(deltaTime) != WorldReconnect::Status::GaveUp) return;
    disconnectNotice_ = "You have been disconnected from the server. Could not reconnect: " +
                        reconnect_->failure();
    reconnect_.reset();
    // The world ends as a disconnect always ended it: the interface hears the
    // character leave, then the login screen.
    transitions_.leave(WorldTransitionQueue::Destination::Login);
}

void ClientLoop::reenterWorld(const game::events::WorldEntered& entry) {
    if (!requireTransitions("re-enter the world")) return;
    LOG_INFO("Reconnected after ", reconnect_->elapsed(), " s on attempt ", reconnect_->attempt(),
             "; the server puts the character on map ", entry.mapId);
    reconnect_.reset();
    auto* map = currentMap();
    if (!map || map->mapId() != entry.mapId || state_ != AppState::IN_GAME) {
        // Somewhere else, or a load that never finished: the character's
        // session ends with its map, and the entry is an ordinary one.
        LOG_INFO("Reconnect: map ", map ? static_cast<int>(map->mapId()) : -1, " is not where the "
                 "server has the character; entering map ", entry.mapId, " as at a login");
        heldReleaseIn_ = -1.0f;
        session_.reset();
        if (services_.worldEntry && services_.loader && services_.spawner)
            services_.worldEntry->enterWorld(entry.mapId, entry.x, entry.y, entry.z, true);
        return;
    }
    // The same map: the next session takes it whole - no load, no loading
    // screen - and the old one ends, writing its interface's saved variables.
    auto kept = session_->releaseMap();
    session_.reset();
    beginCharacterSession();
    session_->adoptMap(std::move(kept));
    // The server's position stands.
    if (services_.worldEntry)
        services_.worldEntry->placeAfterReconnect(entry.mapId, entry.x, entry.y, entry.z);
    // The new interface loads, and hears the login, in the next Interface phase.
    session_->noteMapReady();
    heldReleaseIn_ = kHeldReleaseSeconds;
}

void ClientLoop::updateInterface(float deltaTime, const FramePlan& plan) {
    if (observer_) observer_->interfaceStarting(plan);
    // A map finished loading: the character's interface is built at the first
    // entry and told of every later one.
    if (session_ && !worldEnding()) session_->enterWorldInterface();
    services_.frontEnd.showReconnectStatus(reconnect_ ? reconnect_->statusText() : std::string());
    // Before the events go out, so the UPDATE_BINDINGS LoadBindings raises
    // reaches the buttons in this frame.
    if (session_) session_->applyChangedKeyBindings();
    services_.game.dispatchAddonEvents();
    interfaceInput_.dispatch(session_.get(), services_.playerView.getCameraController(),
                             [this](const SDL_Event& event) { services_.frontEnd.keyForClient(event); },
                             [this](ClientShortcut shortcut) { services_.frontEnd.clientShortcut(shortcut); });
    services_.frontEnd.afterInterfaceInput();
    // A lost mobile surface has no frame to build the screens in. Network and
    // simulation still run, and the single Pace phase limits the background loop.
    if (!services_.frontEnd.canDrawInterface()) {
        if (worldEndsThisFrame(plan)) leaveWorldInterface();
        return;
    }
    services_.frontEnd.updateScreens(deltaTime);
    // A reload asked for by the interface. Done here, between frames, because
    // the request comes from inside the Lua state that the reload destroys -
    // performing it at the call site would free the machinery mid-call. Both
    // ReloadUI and this client's own /reload ask the session for it.
    if (session_) session_->reloadInterfaceIfRequested();
    if (plan.world || plan.loading) {
        if (auto* addons = worldInterface()) addons->update(deltaTime);
    }
    services_.frontEnd.drawInterface(deltaTime, plan);
    services_.game.dispatchAddonEvents();
    if (worldEndsThisFrame(plan)) leaveWorldInterface();
}

void ClientLoop::syncPresentation(float deltaTime, const FramePlan& plan) {
    // Everything the frame asked of the scene first: the controls below stand
    // on this frame's transports and walk into this frame's spawns.
    services_.frontEnd.applySceneChanges();
    // The player's controls move the camera whether or not anything draws it:
    // a client that draws nothing still walks, and still tells the server.
    if (plan.world || plan.loading) services_.playerView.updateControls(deltaTime);
    services_.frontEnd.present(deltaTime, plan);
    if (plan.loading && services_.loader)
        services_.loader->observeReadiness(services_.frontEnd.scenePending());
}

void ClientLoop::leaveWorldInterface() {
    if (!session_) return;
    session_->leaveWorldInterface();
}

void ClientLoop::leaveWorldForShutdown() {
    ScopedPhase interfaceShutdown(services_.phase, Phase::Interface);
    leaveWorldInterface();
}

void ClientLoop::endCharacter() {
    session_.reset();
}

void ClientLoop::enterScene(uint32_t mapId) {
    if (!requireTransitions("enter offline scene")) return;
    if (!services_.spawner) {
        LOG_ERROR("Offline scene on map ", mapId, " not entered: there is no entity spawner "
                  "(the game data did not load); nothing is placed");
        return;
    }
    // No character stands in it, so its session has no interface.
    session_ = std::make_unique<CharacterSession>(characterSessionServices(),
                                                  CharacterSession::Identity{},
                                                  CharacterSession::Interface::None);
    session_->enterScene(mapScopeServices(), mapId).adoptRenderers();
}

void ClientLoop::beginCharacterSession() {
    if (!requireTransitions("begin character session")) return;
    const uint64_t guid = services_.game.getPlayerGuid();
    // A map change keeps the character. A character whose interface was
    // already told it left the world is not in it any more, whatever the
    // guid: it is entering again and gets a session of its own.
    if (session_ && session_->identity().guid == guid && !session_->interfaceHasLeft()) return;
    if (session_) {
        // No logout in between - a different character, or the same one
        // coming back after its interface was taken down.
        LOG_WARNING("Character 0x", std::hex, guid, " entering the world while 0x",
                    session_->identity().guid, std::dec,
                    " still has a session; ending that one first");
        session_.reset();
    }
    // The name keys the per-character saved variables; see characterIdentityOf.
    session_ = std::make_unique<CharacterSession>(
        characterSessionServices(),
        characterIdentityOf(guid, services_.game.getCharacters(), services_.game.lookupName(guid)),
        services_.sessionInterface);
}

void ClientLoop::processWorldTransitions() {
    if (!requireTransitions("apply world transitions")) return;
    // Warmup can receive a leave after Interface has finished. Preserve it until
    // the next Interface can deliver PLAYER_LEAVING_WORLD against the live world.
    // The same for a return after a reconnect, whose old interface leaves.
    if ((worldEnding() || reentryPending()) && session_ && session_->interfaceHoldsWorld()) return;
    if (requestedState_) {
        const auto next = *requestedState_;
        requestedState_.reset();
        if (!transitions_.leaving()) setState(next);
    }
    // The only transition phase, called after render and submit. Applying
    // NEW_WORLD may announce WorldEntered; consume it here too.
    using Destination = WorldTransitionQueue::Destination;
    while (auto request = transitions_.takeNext()) {
        if (auto* leave = std::get_if<WorldTransitionQueue::Leave>(&*request)) {
            if (leave->destination == Destination::Exit) {
                LOG_INFO("Logout complete - quitting after frame submission");
                services_.frontEnd.exiting();
                services_.window.requestClose();
                return;
            }
            if (leave->destination == Destination::Reconnect) {
                // An attempt's own connection closing: the attempt sees it.
                if (reconnect_) continue;
                if (keepsWorldThroughReconnect(leave->characterGuid)) {
                    beginReconnecting(leave->characterGuid);
                    return;
                }
                // Connection-screen failures have no world to release; its
                // retry controller observes the disconnected session itself.
                if (state_ != AppState::IN_GAME && state_ != AppState::LOADING_WORLD && !session_ &&
                    leave->characterGuid == 0)
                    continue;
                LOG_WARNING("Disconnected from the world server; returning to login after frame submission");
                disconnectNotice_ = "You have been disconnected from the server.";
            }
            performLogoutToLogin(leave->destination == Destination::CharacterSelect);
            return;
        }
        if (std::holds_alternative<game::events::WorldTransferStarted>(*request)) {
            if (services_.game.getMovementHandler()->hasPendingWorldTransfer())
                services_.game.getMovementHandler()->applyPendingWorldTransfer();
        } else if (auto* entry = std::get_if<game::events::WorldEntered>(&*request)) {
            if (reconnect_) {
                reenterWorld(*entry);
            } else if (services_.worldEntry && services_.loader && services_.spawner) {
                services_.worldEntry->enterWorld(entry->mapId, entry->x, entry->y, entry->z,
                                                 entry->isInitialEntry);
            } else if (!std::exchange(reportedNoLoader_, true)) {
                LOG_ERROR("World entry to map ", entry->mapId, " not applied: the client has no "
                          "world loader (the game data did not load); the character stays where "
                          "the screens are");
            }
        }
    }
    if (!services_.loader || !services_.spawner) return;
    auto& loader = *services_.loader;
    if (auto entry = loader.takePendingEntry()) beginWorldLoading(*entry);
    if (state_ == AppState::LOADING_WORLD) {
        if (loader.stage() == WorldLoader::Stage::Prepared && loader.initializeScene())
            prepareWorldPlayer();
        if (loader.stage() == WorldLoader::Stage::GeometryReady) finishWorldGeometry();
        if (loader.status().result == WorldLoadResult::Ready) completeWorldLoading();
    }
}

void ClientLoop::performLogoutToLogin(bool keepConnection) {
    if (!requireTransitions("world logout")) return;
    LOG_INFO("Logout requested");

    // Back to the character list when the server completed a logout and the
    // connection is still there; to the login screen otherwise. This always
    // went to the login screen, closing the connection the server had just
    // kept open for the character list - and a new session asked for straight
    // after can be refused while the old one is still closing on the server.
    keepConnection = keepConnection && services_.game.isConnected();

    if (keepConnection) services_.game.returnToCharacterSelect();
    else services_.game.disconnect();
    // A reconnect under way, or entities held after one, go with the world.
    reconnect_.reset();
    heldReleaseIn_ = -1.0f;

    // The character leaves: their map and everything drawn on it, their
    // interface (which writes their saved variables on the way out), their
    // presenters and frame state - all of it ends with their session, and
    // nothing is left for the next character to find.
    session_.reset();
    if (services_.loader) services_.loader->discardPendingEntry();

    services_.frontEnd.characterLeft(keepConnection);

    if (keepConnection) {
        // Same realm, same account: the character screen asks for the list
        // again on its own once the handler is READY.
        setState(AppState::CHARACTER_SELECTION);
        return;
    }

    setState(AppState::AUTHENTICATION);

    // Said on the screen the player lands on, because there is nowhere to say
    // it on the way out: the world is being torn down as this runs.
    if (!disconnectNotice_.empty()) {
        services_.frontEnd.showDisconnectNotice(disconnectNotice_);
        disconnectNotice_.clear();
    }
}

void ClientLoop::beginWorldLoading(WorldLoader::Entry entry) {
    if (!requireTransitions("enter loading world")) return;
    const auto [mapId, x, y, z] = entry;
    (void)mapId;
    // The character entering: their session when this is the first map since
    // they logged in, the one already standing on a map change.
    beginCharacterSession();
    // The previous map - finished, or a load this entry supersedes, even on
    // the same map - ends here: its work is cancelled and joined, its streaming
    // stopped and its instances released before the next scope binds the
    // spawner and the renderers.
    session_->enterMap(mapScopeServices(), *services_.loader, entry);

    auto& game = services_.game;
    game.cancelFollow();
    game.getMovementHandler()->stopInputMovement();
    if (auto* camera = services_.playerView.getCameraController()) {
        const auto spawn = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(x, y, z)));
        const auto yaw = coords::canonicalToCharacterYawDeg(game.getMovementInfo().orientation);
        camera->clearMovementInputs();
        camera->releaseMouseCapture();
        camera->setOnlineMode(true);
        camera->setDefaultSpawn(spawn, yaw, -15.0f);
        camera->reset();
        camera->setEnabled(false);
    }
    services_.frontEnd.mapEntryBegun(entry);
    if (services_.worldEntry) services_.worldEntry->beginMapEntry();
    setState(AppState::LOADING_WORLD);
}

void ClientLoop::prepareWorldPlayer() {
    if (!requireTransitions("prepare loading player")) return;
    auto& loader = *services_.loader;
    // The scene's renderers exist now - built by the first entry, found by
    // later ones - and this map's scope takes its instances and terrain.
    currentMap()->adoptRenderers();
    const auto entry = loader.entry();
    services_.frontEnd.mapRenderersReady(entry, loader.mapName());

    // The renderer may have created the camera's world dependencies just now.
    if (auto* camera = services_.playerView.getCameraController()) {
        const glm::vec3 spawnRender =
            coords::canonicalToRender(coords::serverToCanonical(glm::vec3(entry.x, entry.y, entry.z)));
        const float yaw = coords::canonicalToCharacterYawDeg(services_.game.getMovementInfo().orientation);
        camera->setOnlineMode(true);
        camera->setDefaultSpawn(spawnRender, yaw, -15.0f);
        camera->reset();
        camera->setEnabled(false);
    }
    // NPC death, respawn and swing are presented by AnimationCallbackHandler,
    // which owns those callbacks from setup. Registering them again here for
    // every map replaced its versions (and the rise animation) silently, and
    // left the spawn presentation of a map scope's first objects without them.
}

void ClientLoop::finishWorldGeometry() {
    if (!requireTransitions("finish loading geometry")) return;
    auto& loader = *services_.loader;
    const auto entry = loader.entry();
    if (loader.isInstance()) services_.frontEnd.placeOnInstanceFloor(entry);
    if (auto* camera = services_.playerView.getCameraController()) camera->reset();
    services_.frontEnd.mapGeometryReady(entry);
    loader.beginWarmup();
}

void ClientLoop::completeWorldLoading() {
    if (!requireTransitions("complete loading world")) return;
    auto& loader = *services_.loader;
    if (!loader.finish()) return;
    if (auto* camera = services_.playerView.getCameraController()) {
        camera->clearMovementInputs();
        camera->suppressMovementFor(0);
        // The old warmup advanced this ten-second entry grace with its frames.
        camera->suspendGravityFor(std::max(0.0f, 10.0f - loader.warmupSeconds()));
        camera->setEnabled(true);
        camera->startIntroPan(2.8f, 140.0f);
    }
    setState(AppState::IN_GAME);
    // The interface hears of it in the next Interface phase.
    session_->noteMapReady();
}

} // namespace wowee::core

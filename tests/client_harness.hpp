#pragma once

/// The client's frame and its transitions, with no window, no GPU and no
/// server: GameHarness's game, driven the way Application drives it.
///
/// A ClientLoop - the same frame and the same world transitions the client
/// runs - over stand-ins for what it does not own: a window that is a flag, an
/// input that is a queue the test fills, and no front end at all (nothing is
/// drawn, played or shown). Under it is the client's own part of the world:
/// the player's camera and controls (PlayerView, with a camera of its own),
/// the entity spawner, the world loader with no scene to build, the world
/// entry handler, and an interface per character - a real Lua state that hears
/// events and OnUpdate and loads no FrameXML (CharacterSession::EventsOnly).
///
/// So a test can take the path a player does - log in, load, move, change
/// map, log out, close the window - one frame at a time, and read what the
/// client did: the application state, whether a character's session and map
/// stand, what the spawner was asked to present, what the interface heard,
/// and what the game sent the server (GameHarness's records).
///
/// The world loader is timed by a clock that advances a frame's time per
/// frame, so a load's warmup takes its two seconds of frames and no wall time.
///
/// The login server is a stand-in too: a lost session is renewed, refused or
/// not reached as the test says (setRenewal), and the world server the client
/// connects to again is GameHarness's, on the new connection.
///
/// The game's FrameXML channel belongs to the loop, which routes it to the
/// character's interface: GameHarness::addonEvents records nothing here. What
/// the interface heard is interfaceEvents().

#include "game_harness.hpp"

#include "core/client_loop.hpp"
#include "core/entity_spawn_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/frame_observer.hpp"
#include "core/player_view.hpp"
#include "core/world_entry_callback_handler.hpp"
#include "core/world_loader.hpp"
#include "addons/addon_manager.hpp"
#include "rendering/camera.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;

namespace wowee::test {

class ClientHarness {
public:
    ClientHarness();
    ~ClientHarness();
    ClientHarness(const ClientHarness&) = delete;
    ClientHarness& operator=(const ClientHarness&) = delete;

    /// The server's side: packets in, what the game sent back.
    GameHarness& server() { return game_; }
    game::GameHandler& game() { return game_.game(); }

    // ---- Driving it ---------------------------------------------------------

    /// One frame of the client, as Application's loop runs it: nothing when
    /// the window has closed. False then.
    bool frame();
    /// Frames until `done` holds, or fails the test after `maxFrames`.
    void frameUntil(const std::function<bool()>& done, int maxFrames, const char* what);
    /// This much frame time, a frame at a time.
    void advance(float seconds);
    /// Frames until what the server sent has been read and handled.
    void frameUntilHandled();

    /// What the server sends in the same burst as the login's
    /// SMSG_LOGIN_VERIFY_WORLD - before the client has entered any map.
    using WithLogin = std::function<void()>;

    /// The client's real path into the world: connect, authenticate, list the
    /// characters, enter the world with `player` and load the map. Ends
    /// IN_GAME with the character's interface told it entered.
    void enterWorld(const PlayerSpec& player = {}, const WithLogin& withLogin = {});
    /// The same, stopping in the first LOADING_WORLD frame.
    void beginEnteringWorld(const PlayerSpec& player = {}, const WithLogin& withLogin = {});
    /// From LOADING_WORLD: frames until the world is entered and the interface
    /// has been told.
    void finishEnteringWorld();
    /// From the character screen: enter the world with another character of
    /// the account, `characters` being what the server lists.
    void enterWorldFromCharacterScreen(const PlayerSpec& player,
                                       const std::vector<PlayerSpec>& characters);
    /// The server takes the character to another map (SMSG_NEW_WORLD).
    void receiveNewWorld(uint32_t mapId, float x, float y, float z);
    /// The server's half of a reconnect attempt, once the client has begun
    /// it: greet the new connection, take its AUTH_SESSION, list `player`,
    /// take the character's login and put it in the world where `player`
    /// says. Ends with the login handled; the client applies the entry in
    /// that frame's Transitions.
    void answerReconnect(const PlayerSpec& player, const WithLogin& withLogin = {});
    /// A creature the server shows the client: SMSG_UPDATE_OBJECT with one
    /// create block, a unit with this display at this position.
    void receiveCreature(uint64_t guid, uint32_t displayId, float x, float y, float z);

    /// The player closes the window; the next frame's Input reads it.
    void closeWindow();
    /// A key goes down or comes up, with these modifiers held; the next
    /// frame's Input reads it.
    void pressKey(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE);
    void releaseKey(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE);

    /// What Application does on the way out: the interface hears the
    /// character leave, then the character's session ends.
    void shutdown();

    /// Run `code` in the character's interface, in an Interface scope as Lua
    /// requires. Fails the test when there is no interface or the code raises.
    void runInInterface(const std::string& code);
    /// The same for an expression, answered as a boolean.
    bool interfaceAnswers(const std::string& expression);

    // ---- Reading it ---------------------------------------------------------

    /// The character last logged in.
    [[nodiscard]] const PlayerSpec& player() const { return player_; }
    [[nodiscard]] core::AppState state() const { return loop_->state(); }
    [[nodiscard]] core::CharacterSession* session() { return loop_->session(); }
    [[nodiscard]] core::MapScope* map() { return loop_->currentMap(); }
    [[nodiscard]] core::WorldLoader& loader() { return *loader_; }
    [[nodiscard]] core::EntitySpawner& spawner() { return *spawner_; }
    [[nodiscard]] rendering::CameraController& camera() { return *playerView_->getCameraController(); }
    [[nodiscard]] core::ClientLoop& loop() { return *loop_; }
    /// Whether the loop would run another frame.
    [[nodiscard]] bool running() const { return !windowClosed_; }
    /// How many frames have run to the end of their Interface phase.
    [[nodiscard]] uint64_t interfaceFrames() const { return interfaceFrames_; }
    /// Whether the client has asked the spawner to present this creature on
    /// the current map: queued to be built, or built.
    [[nodiscard]] bool presentsCreature(uint64_t guid) const;
    /// Where the character is drawn: what the renderer's character position
    /// is in the client - the camera's follow target, moved by the controls.
    /// Render coordinates.
    [[nodiscard]] const glm::vec3& characterPosition() const { return characterPosition_; }
    /// What the client says over the world while it reconnects; empty when
    /// it does not.
    [[nodiscard]] const std::string& reconnectStatus() const { return reconnectStatus_; }
    /// What the last frame's Render phase was handed to draw.
    [[nodiscard]] core::FrameView renderedView() const { return renderedView_; }
    /// Every one of the client's own shortcuts the front end was told of, in
    /// order - what Application acts on (Ctrl+` shows the performance HUD).
    [[nodiscard]] const std::vector<core::ClientShortcut>& clientShortcuts() const {
        return clientShortcuts_;
    }

    /// How the login server answers the client's renewals of a lost session
    /// from now on: renewed (the default), refused, or not reached.
    void setRenewal(core::WorldSessionRenewal::Result result, const std::string& failure = {});
    /// How many renewals the client has begun.
    [[nodiscard]] int renewalsBegun() const;
    /// Every event a character's interface heard, in order, across characters;
    /// each is "<character>:<event>".
    [[nodiscard]] const std::vector<std::string>& interfaceEvents() const { return interfaceEvents_; }
    /// Those one character's interface heard.
    [[nodiscard]] std::vector<std::string> interfaceEventsOf(const std::string& character) const;

private:
    class Window;
    class Input;
    class FrontEnd;
    class Observer;
    class Renewal;
    friend class Observer;
    friend class FrontEnd;

    /// At the start of each Interface phase: a new character's interface gets
    /// the recorder that writes interfaceEvents().
    void watchInterface();
    void connectAndList(const PlayerSpec& player);
    void logIn(const PlayerSpec& player, const WithLogin& withLogin);

    std::filesystem::path scratch_;
    GameHarness game_;
    PlayerSpec player_;
    core::Phase phase_ = core::Phase::Idle;
    std::chrono::steady_clock::time_point now_;
    bool windowClosed_ = false;
    std::deque<SDL_Event> pendingInput_;
    uint64_t interfaceFrames_ = 0;
    std::vector<std::string> interfaceEvents_;
    /// The Lua state the recorder was put into, and whose it is.
    const void* recordedState_ = nullptr;
    std::string recordedCharacter_;
    glm::vec3 characterPosition_{0.0f};
    std::string reconnectStatus_;
    core::FrameView renderedView_ = core::FrameView::Login;
    std::vector<core::ClientShortcut> clientShortcuts_;

    ImGuiContext* imgui_ = nullptr;
    rendering::Camera viewCamera_;
    std::unique_ptr<core::PlayerView> playerView_;
    std::unique_ptr<core::EntitySpawner> spawner_;
    std::unique_ptr<core::EntitySpawnCallbackHandler> spawnCallbacks_;
    std::unique_ptr<core::WorldLoader> loader_;
    std::unique_ptr<core::WorldEntryCallbackHandler> worldEntry_;
    std::unique_ptr<addons::AddonCatalog> catalog_;
    std::unique_ptr<Window> window_;
    std::unique_ptr<Input> input_;
    std::unique_ptr<FrontEnd> frontEnd_;
    std::unique_ptr<Renewal> renewal_;
    std::unique_ptr<core::ClientLoop> loop_;
};

} // namespace wowee::test

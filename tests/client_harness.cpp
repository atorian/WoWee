#include "client_harness.hpp"

#include <catch_amalgamated.hpp>  // test_support.hpp's REQUIRE
#include "test_support.hpp"

#include "addons/lua_engine.hpp"
#include "core/character_session.hpp"
#include "core/coordinates.hpp"
#include "core/env.hpp"
#include "core/frame_phase.hpp"
#include "core/map_scope.hpp"
#include "game/entity.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packet_data.hpp"
#include "rendering/camera_controller.hpp"

#include <imgui.h>

#include <algorithm>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <stdexcept>

namespace wowee::test {

namespace {

constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr auto kFrameDuration = std::chrono::microseconds(16'667);

/// A directory of its own, and the client's settings, addon choices and saved
/// variables kept in it: nothing a test does reaches the player's.
std::filesystem::path prepareScratch() {
    const auto dir = scratchDir("wowee_client_harness");
    const std::string config = (dir / "config").string();
    std::filesystem::create_directories(config);
    core::setEnvVar("WOWEE_CONFIG_ROOT", config.c_str());
    return dir;
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("ClientHarness: " + what);
}

/// What the recorder hears: the world's own announcements to the interface.
/// RegisterAllEvents is not offered by this client's frames, so they are
/// named.
constexpr const char* kRecordedEvents[] = {
    "VARIABLES_LOADED", "PLAYER_LOGIN", "PLAYER_ENTERING_WORLD", "PLAYER_LEAVING_WORLD",
};

} // namespace

// ---- The stand-ins ----------------------------------------------------------

/// A window that is a flag: closed when the player asks or the client does.
/// Each frame is a sixtieth of a second, and the world loader's clock moves
/// with it.
class ClientHarness::Window final : public core::ClientWindow {
public:
    explicit Window(ClientHarness& h) : h_(h) {}
    [[nodiscard]] bool shouldClose() const override { return h_.windowClosed_; }
    void requestClose() override { h_.windowClosed_ = true; }
    float beginFrame() override {
        h_.now_ += kFrameDuration;
        return kFrameSeconds;
    }
    void pace() override {}

private:
    ClientHarness& h_;
};

/// The player's input: what the test queued, read in the Input phase the way
/// the client reads SDL's queue - a close request to the window, what the
/// interface hears to the interface.
class ClientHarness::Input final : public core::ClientInput {
public:
    explicit Input(ClientHarness& h) : h_(h) {}
    void pump(float, const core::FramePlan&, core::InterfaceInput& forInterface) override {
        while (!h_.pendingInput_.empty()) {
            const SDL_Event event = h_.pendingInput_.front();
            h_.pendingInput_.pop_front();
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                h_.window_->requestClose();
                continue;
            }
            if (core::InterfaceInput::takes(event)) forInterface.push(event);
        }
    }

private:
    ClientHarness& h_;
};

/// A client that draws nothing, plays nothing and shows no screens. What it
/// would draw the character at is the harness's characterPosition, the
/// camera's follow target from the moment the map's renderers are up, as the
/// renderer's character position is in the client.
class ClientHarness::FrontEnd final : public core::ClientFrontEnd {
public:
    explicit FrontEnd(ClientHarness& h) : h_(h) {}
    void updateLogin(float, const core::FramePlan&) override {}
    void prepareFrame() override {}
    void updateWorld(float) override {}
    void streamScene(float, const core::FramePlan&) override {}
    void showLoadProgress(const core::WorldLoadStatus&) override {}
    void afterInterfaceInput() override {}
    [[nodiscard]] bool canDrawInterface() const override { return true; }
    void updateScreens(float) override {}
    void drawInterface(float, const core::FramePlan&) override {}
    void keyForClient(const SDL_Event&) override {}
    void clientShortcut(core::ClientShortcut shortcut) override {
        h_.clientShortcuts_.push_back(shortcut);
    }
    void present(float, const core::FramePlan&) override {}
    /// Nothing is built for the scene, so nothing is ever still being built.
    [[nodiscard]] bool scenePending() const override { return false; }
    void render(const core::FramePlan& plan) override { h_.renderedView_ = plan.view; }
    void afterTransitions() override {}
    [[nodiscard]] bool profilingStages() const override { return false; }
    void noteStageTime(const char*, float) override {}
    void frameFinished() override {}
    void mapEntryBegun(const core::WorldLoader::Entry&) override {}
    void mapRenderersReady(const core::WorldLoader::Entry& entry, const std::string&) override {
        h_.characterPosition_ = core::coords::canonicalToRender(
            core::coords::serverToCanonical(glm::vec3(entry.x, entry.y, entry.z)));
        h_.camera().setFollowTarget(&h_.characterPosition_);
    }
    void placeOnInstanceFloor(const core::WorldLoader::Entry&) override {}
    void mapGeometryReady(const core::WorldLoader::Entry&) override {}
    void enteredWorld() override {}
    void characterScreenShown() override {}
    void characterLeft(bool) override {}
    void showDisconnectNotice(const std::string&) override {}
    void showReconnectStatus(const std::string& status) override { h_.reconnectStatus_ = status; }
    void exiting() override {}

private:
    ClientHarness& h_;
};

/// The login server, as far as renewing a lost session goes: it answers as
/// the test says, and the world server is where it was.
class ClientHarness::Renewal final : public core::WorldSessionRenewal {
public:
    void begin(const game::WorldLogin& last) override {
        last_ = last;
        ++begun_;
    }
    Result poll(float) override { return result_; }
    [[nodiscard]] game::WorldLogin login() const override { return last_; }
    [[nodiscard]] std::string failure() const override { return failure_; }
    void cancel() override {}

    Result result_ = Result::Renewed;
    std::string failure_;
    game::WorldLogin last_;
    int begun_ = 0;
};

/// The harness's own view of the frames: each Interface phase, a character's
/// new interface gets the recorder before it hears anything.
class ClientHarness::Observer final : public core::FrameObserver {
public:
    explicit Observer(ClientHarness& h) : h_(h) {}
    void interfaceStarting(const core::FramePlan&) override {
        ++h_.interfaceFrames_;
        h_.watchInterface();
    }

private:
    ClientHarness& h_;
};

// ---- The harness ------------------------------------------------------------

ClientHarness::ClientHarness()
    : scratch_(prepareScratch()),
      game_(scratch_ / "account_data"),
      now_(std::chrono::steady_clock::now()) {
    // The camera's controls and the interface's input ask ImGui whether one of
    // the client's own screens has the keyboard or the mouse. None do here.
    imgui_ = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);

    playerView_ = std::make_unique<core::PlayerView>(viewCamera_, nullptr, nullptr, nullptr,
                                                     game_.events());
    spawner_ = std::make_unique<core::EntitySpawner>(nullptr, nullptr, &game(), nullptr,
                                                     playerView_.get());
    spawnCallbacks_ = std::make_unique<core::EntitySpawnCallbackHandler>(
        *spawner_, nullptr, game(), [this](uint64_t guid) {
            return guid != 0 &&
                   (guid == game().getPlayerGuid() || guid == game().getActiveCharacterGuid());
        });
    spawnCallbacks_->setupCallbacks();
    loader_ = std::make_unique<core::WorldLoader>(phase_, nullptr, nullptr,
                                                  scratch_ / "last_world.cfg",
                                                  [this] { return now_; });
    worldEntry_ = std::make_unique<core::WorldEntryCallbackHandler>(
        nullptr, *playerView_, game(), loader_.get(), spawner_.get(), nullptr, phase_);
    catalog_ = std::make_unique<addons::AddonCatalog>();
    catalog_->setFrameXmlDir((scratch_ / "FrameXML").string());
    catalog_->scan((scratch_ / "AddOns").string());

    window_ = std::make_unique<Window>(*this);
    input_ = std::make_unique<Input>(*this);
    frontEnd_ = std::make_unique<FrontEnd>(*this);
    renewal_ = std::make_unique<Renewal>();

    addons::LuaServices luaServices;
    luaServices.executionPhase = [this] { return phase_; };
    playerView_->bindMovementControls(luaServices);

    core::ClientLoop::Services services{
        .phase = phase_,
        .events = game_.events(),
        .game = game(),
        .window = *window_,
        .input = *input_,
        .frontEnd = *frontEnd_,
        .playerView = *playerView_,
        .loader = loader_.get(),
        .spawner = spawner_.get(),
        .worldEntry = worldEntry_.get(),
        .addonCatalog = catalog_.get(),
        .luaServices = luaServices,
        .renewal = renewal_.get(),
        .sessionInterface = core::CharacterSession::Interface::EventsOnly,
    };
    loop_ = std::make_unique<core::ClientLoop>(std::move(services), core::AppState::AUTHENTICATION);
    loop_->setObserver(std::make_unique<Observer>(*this));
}

ClientHarness::~ClientHarness() {
    // The character and the loop before what they borrow.
    loop_.reset();
    // Then the game's session, as Application ends it on the way out: its
    // disconnect despawns what the game still holds through the callbacks
    // into the spawner, which goes before the game does. Left to the game's
    // destructor, those despawns reached a spawner already destroyed - with
    // creatures in view at the end of a test, a crash one run in thirty.
    game().disconnect();
    ImGui::DestroyContext(imgui_);
}

bool ClientHarness::frame() {
    if (!running()) return false;
    loop_->runFrame();
    return true;
}

void ClientHarness::frameUntil(const std::function<bool()>& done, int maxFrames, const char* what) {
    for (int i = 0; i < maxFrames; ++i) {
        if (done()) return;
        if (!frame()) fail(std::string("the window closed while waiting for ") + what);
    }
    if (!done()) fail(std::string("not done after ") + std::to_string(maxFrames) + " frames: " + what);
}

void ClientHarness::advance(float seconds) {
    for (float passed = 0.0f; passed < seconds; passed += kFrameSeconds) frame();
}

void ClientHarness::frameUntilHandled() {
    // What arrived is read in Network and handled within a time budget, so a
    // burst can take a few frames.
    frame();
    frameUntil([this] { return game().queuedIncomingPacketCount() == 0; }, 1000,
               "the server's packets to be handled");
}

void ClientHarness::enterWorld(const PlayerSpec& player, const WithLogin& withLogin) {
    beginEnteringWorld(player, withLogin);
    finishEnteringWorld();
}

void ClientHarness::beginEnteringWorld(const PlayerSpec& player, const WithLogin& withLogin) {
    connectAndList(player);
    logIn(player, withLogin);
}

void ClientHarness::finishEnteringWorld() {
    // A load's warmup is two seconds of frames at the least.
    frameUntil([this] { return state() == core::AppState::IN_GAME; }, 60 * 30,
               "the client to enter the world");
    // The interface hears of the entry in the next Interface phase.
    frame();
}

void ClientHarness::connectAndList(const PlayerSpec& player) {
    using game::Opcode;
    using game::WorldState;

    // What the realm screen does: connect to the world server and show the
    // character screen.
    const std::vector<uint8_t> sessionKey(40, 0x5A);
    if (!game().connect("harness.test", 8085, sessionKey, "HARNESS", 12340, 1)) fail("connect refused");
    loop_->requestState(core::AppState::CHARACTER_SELECTION);
    frameUntil([this] { return game().getState() == WorldState::CONNECTED; }, 60, "the connection");

    game_.receive(Opcode::SMSG_AUTH_CHALLENGE, GameHarness::authChallengeBody(0xC0FFEE));
    frameUntilHandled();
    if (!game_.lastSent(Opcode::CMSG_AUTH_SESSION)) fail("no CMSG_AUTH_SESSION");
    game_.receive(Opcode::SMSG_AUTH_RESPONSE, Body().u8(0x0C));  // AUTH_OK
    frameUntilHandled();
    if (!game_.lastSent(Opcode::CMSG_CHAR_ENUM)) fail("no CMSG_CHAR_ENUM after AUTH_OK");
    game_.receive(Opcode::SMSG_CHAR_ENUM, GameHarness::charEnumBody(player));
    frameUntilHandled();
    if (game().getState() != WorldState::CHAR_LIST_RECEIVED) fail("character list not taken");
}

void ClientHarness::enterWorldFromCharacterScreen(const PlayerSpec& player,
                                                  const std::vector<PlayerSpec>& characters) {
    if (state() != core::AppState::CHARACTER_SELECTION) fail("not at the character screen");
    // The character screen asks for the list again once the game is ready.
    game_.receive(game::Opcode::SMSG_CHAR_ENUM, GameHarness::charEnumBody(characters));
    frameUntilHandled();
    logIn(player, {});
    finishEnteringWorld();
}

void ClientHarness::logIn(const PlayerSpec& player, const WithLogin& withLogin) {
    using game::Opcode;
    player_ = player;
    game_.clearRecords();
    // What the character screen's Enter World does.
    game().selectCharacter(player.guid);
    if (!game_.lastSent(Opcode::CMSG_PLAYER_LOGIN)) fail("no CMSG_PLAYER_LOGIN");
    game_.receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, GameHarness::loginVerifyWorldBody(player));
    if (withLogin) withLogin();
    frameUntil([this] { return state() == core::AppState::LOADING_WORLD; }, 60,
               "the client to begin loading the world");
}

void ClientHarness::receiveNewWorld(uint32_t mapId, float x, float y, float z) {
    game_.receive(game::Opcode::SMSG_NEW_WORLD, Body().u32(mapId).f32(x).f32(y).f32(z).f32(0.0f));
}

void ClientHarness::answerReconnect(const PlayerSpec& player, const WithLogin& withLogin) {
    using game::Opcode;
    using game::WorldState;
    if (!loop_->reconnecting()) fail("the client is not reconnecting");
    // The attempt renews the session and connects again on its own.
    frameUntil([this] { return game_.hasServer() && game().getState() == WorldState::CONNECTED; },
               60 * 30, "the reconnect attempt's connection");
    game_.receive(Opcode::SMSG_AUTH_CHALLENGE, GameHarness::authChallengeBody(0xC0FFEE));
    frameUntilHandled();
    if (!game_.lastSent(Opcode::CMSG_AUTH_SESSION)) fail("no CMSG_AUTH_SESSION on the new connection");
    game_.receive(Opcode::SMSG_AUTH_RESPONSE, Body().u8(0x0C));  // AUTH_OK
    frameUntilHandled();
    game_.receive(Opcode::SMSG_CHAR_ENUM, GameHarness::charEnumBody(player));
    frameUntil([this] { return game_.lastSent(Opcode::CMSG_PLAYER_LOGIN) != nullptr; }, 60,
               "the character's login");
    player_ = player;
    game_.receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, GameHarness::loginVerifyWorldBody(player));
    if (withLogin) withLogin();
    frameUntilHandled();
}

void ClientHarness::setRenewal(core::WorldSessionRenewal::Result result, const std::string& failure) {
    renewal_->result_ = result;
    renewal_->failure_ = failure;
}

int ClientHarness::renewalsBegun() const {
    return renewal_->begun_;
}

void ClientHarness::receiveCreature(uint64_t guid, uint32_t displayId, float x, float y, float z) {
    game_.receive(game::Opcode::SMSG_UPDATE_OBJECT,
                  GameHarness::createObjectBody(
                      guid, game::ObjectType::UNIT,
                      {{game::fieldIndex(game::UF::UNIT_FIELD_HEALTH), 100},
                       {game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH), 100},
                       {game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID), displayId}},
                      x, y, z));
}

void ClientHarness::closeWindow() {
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    pendingInput_.push_back(event);
}

void ClientHarness::pressKey(SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    // Where the key is on a US keyboard, as SDL reports it alongside the key.
    event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
    event.key.mod = modifiers;
    event.key.down = true;
    pendingInput_.push_back(event);
}

void ClientHarness::releaseKey(SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_UP;
    event.key.key = key;
    event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
    event.key.mod = modifiers;
    event.key.down = false;
    pendingInput_.push_back(event);
}

void ClientHarness::shutdown() {
    core::ScopedPhase shuttingDown(phase_, core::Phase::Transitions);
    {
        // A character whose interface has not had an Interface phase yet - a
        // window closed in the frame after the load began - gets the recorder
        // now, before it hears the character leave.
        core::ScopedPhase interfacePhase(phase_, core::Phase::Interface);
        watchInterface();
    }
    loop_->leaveWorldForShutdown();
    loop_->endCharacter();
}

void ClientHarness::runInInterface(const std::string& code) {
    auto* addons = loop_->characterAddons();
    if (!addons) fail("no interface to run Lua in");
    core::ScopedPhase interfacePhase(phase_, core::Phase::Interface);
    if (!addons->getLuaEngine()->executeString(code)) {
        fail("interface Lua failed: " + addons->getLuaEngine()->lastError() + "\n" + code);
    }
}

bool ClientHarness::interfaceAnswers(const std::string& expression) {
    auto* addons = loop_->characterAddons();
    if (!addons) fail("no interface to ask");
    core::ScopedPhase interfacePhase(phase_, core::Phase::Interface);
    return addons->interfaceCommandBoolean(expression);
}

void ClientHarness::layOutInterface() {
    auto* addons = loop_->characterAddons();
    if (!addons) fail("no interface to lay out");
    core::ScopedPhase interfacePhase(phase_, core::Phase::Interface);
    auto* engine = addons->getLuaEngine();
    // The order Application keeps: layout, then visibility, then sizes.
    engine->widgets().layout(1920.0f, 1080.0f);
    engine->updateVisibility();
    engine->updateSizeChanges();
}

bool ClientHarness::presentsCreature(uint64_t guid) const {
    return spawner_->isCreaturePending(guid) || spawner_->isCreatureSpawned(guid);
}

std::vector<std::string> ClientHarness::interfaceEventsOf(const std::string& character) const {
    std::vector<std::string> out;
    const std::string prefix = character + ":";
    for (const auto& e : interfaceEvents_) {
        if (e.compare(0, prefix.size(), prefix) == 0) out.push_back(e.substr(prefix.size()));
    }
    return out;
}

namespace {

int recordInterfaceEvent(lua_State* L) {
    auto* events = static_cast<std::vector<std::string>*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* character = lua_tostring(L, lua_upvalueindex(2));
    const char* event = luaL_checkstring(L, 1);
    events->push_back(std::string(character ? character : "") + ":" + event);
    return 0;
}

} // namespace

void ClientHarness::watchInterface() {
    auto* session = loop_->session();
    auto* addons = session ? session->addonManager() : nullptr;
    auto* engine = addons ? addons->getLuaEngine() : nullptr;
    lua_State* L = engine ? engine->getState() : nullptr;
    if (!L) return;
    // Once per Lua state: a /reload or the next character gets one afresh.
    lua_getglobal(L, "__HarnessRecord");
    const bool installed = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (installed) return;
    lua_pushlightuserdata(L, &interfaceEvents_);
    lua_pushstring(L, session->identity().name.c_str());
    lua_pushcclosure(L, &recordInterfaceEvent, 2);
    lua_setglobal(L, "__HarnessRecord");
    std::string code = "local f = CreateFrame('Frame')\n";
    for (const char* event : kRecordedEvents) code += std::string("f:RegisterEvent('") + event + "')\n";
    code += "f:SetScript('OnEvent', function(_, event) __HarnessRecord(event) end)\n";
    if (!engine->executeString(code)) fail("could not install the interface recorder: " + engine->lastError());
}

} // namespace wowee::test

// The client's half of the world's transitions, end to end: the frame and the
// lifetimes ClientLoop begins and ends - the character's session, the map
// inside it, the load, the interface - driven the way a player drives them,
// one frame at a time, with no window, GPU or server (tests/client_harness.hpp).
//
// Each case is a path a player takes - log in and load, move, change map, log
// out or lose the connection while loading or in the world, close the window,
// log in another character - and reads what the client did: its state, what
// stands of the world, what the spawner was asked to present, what the
// interface heard and what the game told the server.
#include <catch_amalgamated.hpp>

#include "client_harness.hpp"

#include "core/coordinates.hpp"
#include "core/map_scope.hpp"
#include "core/world_reconnect.hpp"
#include "game/movement_flags.hpp"
#include "game/opcodes.hpp"
#include "rendering/camera_controller.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::core::AppState;
using Renewal = wowee::core::WorldSessionRenewal::Result;
using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::ClientHarness;
using wowee::test::PlayerSpec;

namespace {

/// What bindings.xml gives the interface for walking forward: the press holds
/// MOVEFORWARD and the release lets go. The client's interface without
/// FrameXML has no binding scripts, so a test that walks brings its own.
void bindMoveForward(ClientHarness& h) {
    h.runInInterface(
        "__WoweeBindingScripts = __WoweeBindingScripts or {}\n"
        "__WoweeBindingRunOnUp = __WoweeBindingRunOnUp or {}\n"
        "__WoweeBindingScripts.MOVEFORWARD = function(keystate)\n"
        "  if keystate == 'down' then MoveForwardStart() else MoveForwardStop() end\n"
        "end\n"
        "__WoweeBindingRunOnUp.MOVEFORWARD = true\n");
}

bool movingForward(ClientHarness& h) {
    using wowee::game::MovementFlags;
    return (h.game().getMovementInfo().flags & static_cast<uint32_t>(MovementFlags::FORWARD)) != 0;
}

/// What the world has left standing of a character: nothing, once they leave.
void requireNothingOfTheWorldStands(ClientHarness& h) {
    CHECK(h.session() == nullptr);
    CHECK(h.map() == nullptr);
    CHECK_FALSE(h.spawner().hasMap());
    CHECK(h.loader().stage() == wowee::core::WorldLoader::Stage::Idle);
}

/// The last thing a character's interface heard.
std::string lastHeardBy(const ClientHarness& h, const std::string& character) {
    const auto events = h.interfaceEventsOf(character);
    return events.empty() ? std::string() : events.back();
}

} // namespace

TEST_CASE("the client's real path into the world ends in the world with the interface told",
          "[client][harness]") {
    ClientHarness h;
    h.enterWorld();
    CHECK(h.state() == AppState::IN_GAME);
    REQUIRE(h.session());
    CHECK(h.session()->identity().name == "Harness");
    REQUIRE(h.map());
    CHECK(h.map()->mapId() == h.player().mapId);
    CHECK(h.loader().stage() == wowee::core::WorldLoader::Stage::Finished);
    const auto heard = h.interfaceEventsOf("Harness");
    CHECK(std::count(heard.begin(), heard.end(), "PLAYER_ENTERING_WORLD") >= 1);
    CHECK(std::count(heard.begin(), heard.end(), "PLAYER_LEAVING_WORLD") == 0);
}

TEST_CASE("closing the window while the world loads ends the loop before anything else runs",
          "[client][harness][loading]") {
    ClientHarness h;
    h.beginEnteringWorld();
    REQUIRE(h.state() == AppState::LOADING_WORLD);
    REQUIRE(h.map());

    // The server still talks; the player closes the window.
    h.server().clearRecords();
    h.server().receive(Opcode::SMSG_TIME_SYNC_REQ, Body().u32(7));
    h.closeWindow();
    const auto interfaceFramesBefore = h.interfaceFrames();
    CHECK(h.frame());

    // That frame read the close in Input and went no further: nothing from the
    // server was read, the interface did not run, the load did not go on.
    CHECK_FALSE(h.running());
    CHECK(h.server().countSent(Opcode::CMSG_TIME_SYNC_RESP) == 0);
    CHECK(h.interfaceFrames() == interfaceFramesBefore);
    CHECK(h.state() == AppState::LOADING_WORLD);
    // And the loop runs no other.
    CHECK_FALSE(h.frame());

    // On the way out the interface hears the character leave while the map
    // still stands, then everything of the world goes.
    h.shutdown();
    CHECK(lastHeardBy(h, "Harness") == "PLAYER_LEAVING_WORLD");
    requireNothingOfTheWorldStands(h);
}

TEST_CASE("a key let go while the world loads does not leave the character walking",
          "[client][harness][loading]") {
    ClientHarness h;
    h.enterWorld();
    bindMoveForward(h);

    // W walks: the binding holds MOVEFORWARD and the camera tells the server.
    h.pressKey(SDLK_W);
    h.advance(0.5f);
    REQUIRE(h.server().countSent(Opcode::MSG_MOVE_START_FORWARD) == 1);
    REQUIRE(movingForward(h));

    // The server takes the character to another map while W is held.
    const auto& player = h.player();
    h.server().clearRecords();
    h.receiveNewWorld(1, player.x + 50.0f, player.y, player.z);
    h.frameUntil([&] { return h.state() == AppState::LOADING_WORLD; }, 60, "the map change's load");

    // W comes up during the load.
    h.releaseKey(SDLK_W);
    h.frame();
    REQUIRE(h.state() == AppState::LOADING_WORLD);

    h.finishEnteringWorld();
    h.advance(4.0f);
    // Nothing started the walk again, and the server was last told it stopped.
    CHECK(h.server().countSent(Opcode::MSG_MOVE_START_FORWARD) == 0);
    CHECK_FALSE(movingForward(h));
    CHECK_FALSE(h.camera().isMoving());
    CHECK_FALSE(h.camera().isMovementHeld(wowee::rendering::MovementCommand::Forward));
}

TEST_CASE("the first creature the server shows at login is presented once the map is entered",
          "[client][harness][mapscope]") {
    // Sent with the login, in the same burst as SMSG_LOGIN_VERIFY_WORLD:
    // handled before the client has any map to present it on.
    constexpr uint64_t kCreature = 0xF130000001000042ULL;
    ClientHarness h;
    const PlayerSpec player;
    h.enterWorld(player, [&] {
        h.receiveCreature(kCreature, 4321, player.x + 4.0f, player.y, player.z);
    });
    REQUIRE(h.game().getEntityManager().hasEntity(kCreature));
    CHECK(h.presentsCreature(kCreature));
}

TEST_CASE("a map change ends the old map before the next is built, and the character stays",
          "[client][harness][mapscope]") {
    constexpr uint64_t kCreature = 0xF130000001000051ULL;
    ClientHarness h;
    h.enterWorld();
    const auto& player = h.player();
    h.receiveCreature(kCreature, 4321, player.x + 4.0f, player.y, player.z);
    h.frameUntilHandled();
    REQUIRE(h.presentsCreature(kCreature));
    // The character's interface keeps what it holds across a map change.
    h.runInInterface("MAP_CHANGE_MARK = 1");

    h.receiveNewWorld(1, 1000.0f, 2000.0f, 300.0f);
    // Ending the old map is what lets the new one bind the spawner: built
    // first, the new scope would find it taken and the frame would throw.
    REQUIRE_NOTHROW(h.frameUntil([&] { return h.state() == AppState::LOADING_WORLD; }, 60,
                                 "the map change's load"));
    REQUIRE(h.map());
    CHECK(h.map()->mapId() == 1);
    REQUIRE_NOTHROW(h.finishEnteringWorld());

    // Nothing of the old map is presented on the new one.
    CHECK_FALSE(h.presentsCreature(kCreature));
    // The character did not leave: same interface, told of the entry again.
    CHECK(h.interfaceAnswers("MAP_CHANGE_MARK == 1"));
    const auto heard = h.interfaceEventsOf("Harness");
    CHECK(std::count(heard.begin(), heard.end(), "PLAYER_ENTERING_WORLD") >= 2);
    CHECK(std::count(heard.begin(), heard.end(), "PLAYER_LEAVING_WORLD") == 0);
}

TEST_CASE("logout, and a disconnect that cannot be ridden out, while loading or in the world, "
          "leave nothing of the world",
          "[client][harness][loading]") {
    const bool whileLoading = GENERATE(true, false);
    const bool disconnect = GENERATE(false, true);
    CAPTURE(whileLoading, disconnect);

    ClientHarness h;
    if (whileLoading) {
        h.beginEnteringWorld();
        REQUIRE(h.state() == AppState::LOADING_WORLD);
    } else {
        h.enterWorld();
        REQUIRE(h.state() == AppState::IN_GAME);
    }
    h.server().clearRecords();

    if (disconnect) {
        // The login server will not renew the session: nothing to ride it
        // out with.
        h.setRenewal(Renewal::Refused, "the login server holds no session for this key");
        h.server().server().disconnect();
    } else {
        h.game().requestLogout(false);
        h.server().receive(Opcode::SMSG_LOGOUT_COMPLETE);
    }
    const AppState destination = disconnect ? AppState::AUTHENTICATION : AppState::CHARACTER_SELECTION;
    h.frameUntil([&] { return h.state() == destination; }, 60, "the client to leave the world");

    // The interface heard the character leave while the world stood, and then
    // the character's session and map went.
    CHECK(lastHeardBy(h, "Harness") == "PLAYER_LEAVING_WORLD");
    requireNothingOfTheWorldStands(h);
    if (disconnect) {
        CHECK_FALSE(h.game().isConnected());
    } else {
        // The connection stays for the character list, which is asked again.
        CHECK(h.game().isConnected());
        CHECK(h.server().countSent(Opcode::CMSG_CHAR_ENUM) == 1);
    }

    // Nothing comes back by itself: no map is entered, no session begun.
    h.advance(3.0f);
    CHECK(h.state() == destination);
    requireNothingOfTheWorldStands(h);
    CHECK(h.loop().reconnecting() == nullptr);
}

TEST_CASE("a committed logout stops drawing the world at once, while the world stands until "
          "the server completes it",
          "[client][harness][logout]") {
    // The server answers the logout, then takes every object in view away -
    // out of range, worked through over several frames - and only then sends
    // LOGOUT_COMPLETE. The player is not to watch the world empty out.
    const bool instant = GENERATE(true, false);
    CAPTURE(instant);

    ClientHarness h;
    h.enterWorld();
    REQUIRE(h.renderedView() == wowee::core::FrameView::World);

    h.game().requestLogout(false);
    h.server().receive(Opcode::SMSG_LOGOUT_RESPONSE, Body().u32(0).u8(instant ? 1 : 0));
    h.frameUntilHandled();
    if (!instant) {
        // Twenty seconds the player can still cancel in: the world is drawn.
        h.advance(19.0f);
        CHECK(h.renderedView() == wowee::core::FrameView::World);
        h.advance(1.5f);
    }
    h.frame();

    CHECK(h.renderedView() == wowee::core::FrameView::LeavingWorld);
    // Only hidden: the server has not completed it, and the world stands.
    CHECK(h.state() == AppState::IN_GAME);
    CHECK(h.map() != nullptr);
    CHECK(h.session() != nullptr);

    h.server().receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.frameUntil([&] { return h.state() == AppState::CHARACTER_SELECTION; }, 60,
                 "the client to leave for the character list");
    CHECK(lastHeardBy(h, "Harness") == "PLAYER_LEAVING_WORLD");
    requireNothingOfTheWorldStands(h);
    h.frame();
    CHECK(h.renderedView() == wowee::core::FrameView::Characters);
}

TEST_CASE("a logout cancelled during its countdown keeps the world drawn",
          "[client][harness][logout]") {
    ClientHarness h;
    h.enterWorld();
    h.game().requestLogout(false);
    h.server().receive(Opcode::SMSG_LOGOUT_RESPONSE, Body().u32(0).u8(0));
    h.frameUntilHandled();
    h.advance(5.0f);
    h.game().cancelLogout();
    // Past where the countdown would have run out.
    h.advance(20.0f);

    CHECK(h.renderedView() == wowee::core::FrameView::World);
    CHECK(h.state() == AppState::IN_GAME);
    CHECK(h.map() != nullptr);
}

TEST_CASE("a committed logout the server never completes ends at the login screen, "
          "not before a hidden world",
          "[client][harness][logout]") {
    ClientHarness h;
    h.enterWorld();
    h.game().requestLogout(false);
    h.server().receive(Opcode::SMSG_LOGOUT_RESPONSE, Body().u32(0).u8(1));
    h.frameUntilHandled();

    // Well past the client's ten seconds, and short of forever.
    h.frameUntil([&] { return h.state() == AppState::AUTHENTICATION; }, 2000,
                 "the client to give up on the logout and leave for the login screen");
    requireNothingOfTheWorldStands(h);
    CHECK_FALSE(h.game().isConnected());
}

TEST_CASE("the interface answers what it is asked, false as well as true",
          "[client][harness][lua]") {
    // The client asks the interface questions whose answer decides what it
    // does next - whether Escape closed one of its panels, before opening the
    // game menu - and read whatever was on top of the Lua stack instead of
    // the answer.
    ClientHarness h;
    h.enterWorld();
    h.runInInterface("ANSWER_ME = 2");
    CHECK(h.interfaceAnswers("ANSWER_ME == 2"));
    CHECK_FALSE(h.interfaceAnswers("ANSWER_ME == 3"));
    CHECK_FALSE(h.interfaceAnswers("nil"));
    CHECK(h.interfaceAnswers("ANSWER_ME"));
}

TEST_CASE("the interface goes on hearing the game and running its frames while the world loads",
          "[client][harness][loading]") {
    // A frame made during the load: its OnUpdate waited for a layout pass,
    // which only a frame drawn over the world has, so it never ran.
    ClientHarness h;
    h.beginEnteringWorld();
    h.runInInterface(
        "LOAD_UPDATES = 0; LOAD_EVENTS = ''\n"
        "local f = CreateFrame('Frame')\n"
        "f:RegisterEvent('WOWEE_HARNESS_EVENT')\n"
        "f:SetScript('OnEvent', function(_, _, arg) LOAD_EVENTS = LOAD_EVENTS .. arg end)\n"
        "f:SetScript('OnUpdate', function() LOAD_UPDATES = LOAD_UPDATES + 1 end)\n");
    h.game().fireAddonEvent("WOWEE_HARNESS_EVENT", {"one"});
    h.game().fireAddonEvent("WOWEE_HARNESS_EVENT", {"two"});
    h.frame();
    h.frame();
    REQUIRE(h.state() == AppState::LOADING_WORLD);
    CHECK(h.interfaceAnswers("LOAD_EVENTS == 'onetwo'"));
    CHECK(h.interfaceAnswers("LOAD_UPDATES >= 2"));
    // A hidden frame does not run.
    h.runInInterface("LOAD_UPDATES = 0; local g = CreateFrame('Frame'); g:Hide()\n"
                     "g:SetScript('OnUpdate', function() LOAD_HIDDEN_RAN = true end)");
    h.frame();
    CHECK(h.interfaceAnswers("LOAD_HIDDEN_RAN == nil"));
}

TEST_CASE("logging in another character carries none of the last one's interface",
          "[client][harness][charactersession]") {
    PlayerSpec alice;
    alice.guid = 0x21;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x22;
    bob.name = "Bob";

    ClientHarness h;
    h.enterWorld(alice);
    // Alice ticks a dungeon in the finder and leaves something in her Lua state.
    h.runInInterface("SetLFGDungeonEnabled(285, true); RELOG_MARK = 'alice'");
    REQUIRE(h.interfaceAnswers("GetLFDChoiceEnabledState()[285] == true"));

    h.game().requestLogout(false);
    h.server().receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.frameUntil([&] { return h.state() == AppState::CHARACTER_SELECTION; }, 60,
                 "the character screen");
    CHECK(lastHeardBy(h, "Alice") == "PLAYER_LEAVING_WORLD");

    h.enterWorldFromCharacterScreen(bob, {alice, bob});
    REQUIRE(h.session());
    CHECK(h.session()->identity().name == "Bob");
    CHECK(h.interfaceAnswers("next(GetLFDChoiceEnabledState()) == nil"));
    CHECK(h.interfaceAnswers("RELOG_MARK == nil"));
    const auto bobHeard = h.interfaceEventsOf("Bob");
    CHECK(std::count(bobHeard.begin(), bobHeard.end(), "PLAYER_ENTERING_WORLD") >= 1);
    CHECK(std::count(bobHeard.begin(), bobHeard.end(), "PLAYER_LEAVING_WORLD") == 0);
}

// ---- A world connection lost and restored --------------------------------------

namespace {

/// The server's end of the connection closes, as a dropped cable or a server
/// restart closes it; frames until the client is reconnecting.
void loseConnection(ClientHarness& h) {
    h.server().server().disconnect();
    h.frameUntil([&] { return h.loop().reconnecting() != nullptr; }, 60,
                 "the client to begin reconnecting");
}

float horizontalDistance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a.x - b.x, a.y - b.y));
}

/// Where the server puts the player, as the client draws it.
glm::vec3 drawnAt(const PlayerSpec& p) {
    namespace coords = wowee::core::coords;
    return coords::canonicalToRender(coords::serverToCanonical(glm::vec3(p.x, p.y, p.z)));
}

}  // namespace

TEST_CASE("a connection lost in the world is ridden out on the same map: the map and what it "
          "shows stay, with no loading screen, and the interface starts again",
          "[client][harness][reconnect]") {
    constexpr uint64_t kWolf = 0xF130000001000061ULL;
    constexpr uint64_t kBoar = 0xF130000001000062ULL;
    ClientHarness h;
    h.enterWorld();
    const PlayerSpec player = h.player();
    h.receiveCreature(kWolf, 4321, player.x + 4.0f, player.y, player.z);
    h.receiveCreature(kBoar, 4322, player.x - 4.0f, player.y, player.z);
    h.frameUntilHandled();
    REQUIRE(h.presentsCreature(kWolf));
    REQUIRE(h.presentsCreature(kBoar));
    h.runInInterface("RECONNECT_MARK = 1");
    const auto* map = h.map();

    loseConnection(h);
    // Still in the world it was in, saying what it is doing from the next
    // Interface phase on.
    h.frame();
    CHECK(h.state() == AppState::IN_GAME);
    CHECK(h.map() == map);
    CHECK(h.reconnectStatus().find("Reconnecting") != std::string::npos);
    CHECK(h.presentsCreature(kWolf));
    CHECK(h.presentsCreature(kBoar));

    // Every frame from here to the end of the hold: never a loading screen,
    // and the wolf - which the server shows again - never gone.
    const auto watch = [&] {
        CHECK(h.state() == AppState::IN_GAME);
        CHECK(h.presentsCreature(kWolf));
    };
    h.answerReconnect(player, [&] {
        h.receiveCreature(kWolf, 4321, player.x + 4.0f, player.y, player.z);
    });
    for (int i = 0; i < 2; ++i) {
        h.frame();
        watch();
    }
    CHECK(h.loop().reconnecting() == nullptr);
    CHECK(h.reconnectStatus().empty());
    CHECK(h.map() == map);
    // The interface is a new one, told of the world as at a login, with
    // nothing of the last one's state.
    CHECK(lastHeardBy(h, "Harness") == "PLAYER_ENTERING_WORLD");
    const auto heard = h.interfaceEventsOf("Harness");
    CHECK(std::count(heard.begin(), heard.end(), "PLAYER_LEAVING_WORLD") == 1);
    CHECK(h.interfaceAnswers("RECONNECT_MARK == nil"));

    // The boar, which the server did not show again, is gone once the
    // login's first burst is in; the wolf stays.
    for (int i = 0; i < 60 * 4; ++i) {
        h.frame();
        watch();
    }
    CHECK_FALSE(h.presentsCreature(kBoar));
    CHECK(h.map() == map);
}

TEST_CASE("a reconnect that finds the character on another map is an ordinary map change",
          "[client][harness][reconnect]") {
    ClientHarness h;
    h.enterWorld();
    PlayerSpec elsewhere = h.player();
    elsewhere.mapId = 1;
    elsewhere.x = 1000.0f;
    elsewhere.y = 2000.0f;
    elsewhere.z = 300.0f;

    loseConnection(h);
    h.answerReconnect(elsewhere);
    h.frameUntil([&] { return h.state() == AppState::LOADING_WORLD; }, 60, "the new map's load");
    REQUIRE(h.map());
    CHECK(h.map()->mapId() == 1);
    CHECK(h.loop().reconnecting() == nullptr);
    h.finishEnteringWorld();
    CHECK(h.state() == AppState::IN_GAME);
    CHECK(h.map()->mapId() == 1);
    CHECK(lastHeardBy(h, "Harness") == "PLAYER_ENTERING_WORLD");
}

TEST_CASE("when every reconnect attempt fails the character leaves the world for the login screen",
          "[client][harness][reconnect]") {
    ClientHarness h;
    h.enterWorld();
    // The world server is down: every attempt's connection fails.
    h.server().setConnectionsRefused(true);
    loseConnection(h);

    // Five attempts, the waits between them growing; then the login screen.
    h.frameUntil([&] { return h.state() == AppState::AUTHENTICATION; }, 60 * 120,
                 "the client to give up");
    CHECK(h.renewalsBegun() == wowee::core::WorldReconnect::kMaxAttempts);
    CHECK(lastHeardBy(h, "Harness") == "PLAYER_LEAVING_WORLD");
    requireNothingOfTheWorldStands(h);
    CHECK(h.loop().reconnecting() == nullptr);
    CHECK(h.reconnectStatus().empty());
}

TEST_CASE("an attempt that timed out is followed by the next at once, not after another wait",
          "[client][harness][reconnect]") {
    ClientHarness h;
    h.enterWorld();
    // The login server does not answer: the first attempt runs out its time.
    h.setRenewal(Renewal::Pending);
    loseConnection(h);
    h.frameUntil([&] { return h.renewalsBegun() == 2; }, 60 * 21, "the second attempt");
    // It began as the first gave up - twenty seconds in, past the five the
    // second may start after - and the network being back is all it needs.
    h.setRenewal(Renewal::Renewed);
    h.answerReconnect(h.player());
    h.frame();
    CHECK(h.loop().reconnecting() == nullptr);
    CHECK(h.state() == AppState::IN_GAME);
}

TEST_CASE("a refused session ends the reconnect at once", "[client][harness][reconnect]") {
    ClientHarness h;
    h.enterWorld();
    h.setRenewal(Renewal::Refused, "the login server holds no session for this key");
    loseConnection(h);
    h.frameUntil([&] { return h.state() == AppState::AUTHENTICATION; }, 60,
                 "the client to give up");
    CHECK(h.renewalsBegun() == 1);
    requireNothingOfTheWorldStands(h);
}

TEST_CASE("a server gone silent, with the socket still open, is reconnected to",
          "[client][harness][reconnect]") {
    ClientHarness h;
    h.enterWorld();
    const auto* map = h.map();
    h.server().setPingsAnswered(false);
    // Nothing comes, and the ping asking about it is not answered.
    h.frameUntil([&] { return h.loop().reconnecting() != nullptr; }, 60 * 30,
                 "the silence to be taken for a lost connection");
    CHECK(h.state() == AppState::IN_GAME);
    CHECK(h.map() == map);
}

TEST_CASE("the character walks while the client reconnects, and stands where the server has it "
          "once back", "[client][harness][reconnect]") {
    ClientHarness h;
    h.enterWorld();
    const PlayerSpec player = h.player();
    bindMoveForward(h);
    loseConnection(h);
    const glm::vec3 lostAt = h.characterPosition();

    // W walks, with no world to tell: the character moves here all the same.
    h.pressKey(SDLK_W);
    h.advance(1.0f);
    h.releaseKey(SDLK_W);
    h.frame();
    CHECK(horizontalDistance(h.characterPosition(), lostAt) > 2.0f);

    // Back in, the server has the character where it last knew it.
    h.answerReconnect(player);
    h.frame();
    REQUIRE(h.loop().reconnecting() == nullptr);
    CHECK(horizontalDistance(h.characterPosition(), drawnAt(player)) < 0.01f);
    const auto& moved = h.game().getMovementInfo();
    const glm::vec3 serverCanonical =
        wowee::core::coords::serverToCanonical(glm::vec3(player.x, player.y, player.z));
    CHECK(glm::length(glm::vec2(moved.x - serverCanonical.x, moved.y - serverCanonical.y)) < 0.01f);
}

TEST_CASE("a cast while the client reconnects is refused at once and never sent",
          "[client][harness][reconnect]") {
    constexpr uint32_t kFireball = 133;
    ClientHarness h;
    h.enterWorld();
    loseConnection(h);
    h.runInInterface(
        "ERRS = ''\n"
        "local f = CreateFrame('Frame')\n"
        "f:RegisterEvent('UI_ERROR_MESSAGE')\n"
        "f:SetScript('OnEvent', function(_, _, msg) ERRS = ERRS .. msg end)\n");

    h.game().castSpell(kFireball, 0);
    h.frame();
    CHECK(h.interfaceAnswers("ERRS == 'No connection to the server.'"));

    h.answerReconnect(h.player());
    h.advance(1.0f);
    REQUIRE(h.loop().reconnecting() == nullptr);
    CHECK(h.server().countSent(Opcode::CMSG_CAST_SPELL) == 0);
}

TEST_CASE("control and the grave key show the performance HUD and reach nothing in the interface; "
          "F1 is the interface's", "[client][harness][input]") {
    using wowee::core::ClientShortcut;
    ClientHarness h;
    h.enterWorld();
    // Every key the test presses bound to a command that notes the press, as
    // a character's bindings bind them - F1 is TARGETSELF on most.
    h.runInInterface(
        "__WoweeBindingScripts = __WoweeBindingScripts or {}\n"
        "HarnessKeysHeard = {}\n"
        "HarnessBindingsBefore = {}\n"
        "for _, key in ipairs({'F1', '`', 'CTRL-`', 'CTRL-SHIFT-`'}) do\n"
        "  local command = 'HARNESSKEY_' .. key\n"
        "  HarnessBindingsBefore[key] = GetBindingAction(key) or ''\n"
        "  __WoweeBindingScripts[command] = function(keystate)\n"
        "    if keystate == 'down' then table.insert(HarnessKeysHeard, key) end\n"
        "  end\n"
        "  SetBinding(key, command)\n"
        "end\n");

    h.pressKey(SDLK_GRAVE, SDL_KMOD_LCTRL);
    h.releaseKey(SDLK_GRAVE, SDL_KMOD_LCTRL);
    h.pressKey(SDLK_GRAVE, SDL_KMOD_RCTRL);
    h.releaseKey(SDLK_GRAVE, SDL_KMOD_RCTRL);
    h.frame();
    // Either control key: shown, then hidden again. The interface heard neither.
    CHECK(h.clientShortcuts() == std::vector<ClientShortcut>{ClientShortcut::TogglePerformanceHud,
                                                             ClientShortcut::TogglePerformanceHud});
    CHECK(h.interfaceAnswers("#HarnessKeysHeard == 0"));

    // F1, the plain grave key and another chord on it are the interface's.
    const auto ctrlShift = static_cast<SDL_Keymod>(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    h.pressKey(SDLK_F1);
    h.releaseKey(SDLK_F1);
    h.pressKey(SDLK_GRAVE);
    h.releaseKey(SDLK_GRAVE);
    h.pressKey(SDLK_GRAVE, ctrlShift);
    h.releaseKey(SDLK_GRAVE, ctrlShift);
    h.frame();
    CHECK(h.clientShortcuts().size() == 2);
    CHECK(h.interfaceAnswers("#HarnessKeysHeard == 3 and HarnessKeysHeard[1] == 'F1' and "
                             "HarnessKeysHeard[2] == '`' and HarnessKeysHeard[3] == 'CTRL-SHIFT-`'"));

    // The binding table outlives the harness; the next case gets it as it was.
    h.runInInterface(
        "for key, command in pairs(HarnessBindingsBefore) do\n"
        "  if command ~= '' then SetBinding(key, command) else SetBinding(key) end\n"
        "end\n");
}

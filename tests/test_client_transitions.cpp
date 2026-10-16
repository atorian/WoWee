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

#include "core/map_scope.hpp"
#include "game/movement_flags.hpp"
#include "game/opcodes.hpp"
#include "rendering/camera_controller.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::core::AppState;
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

TEST_CASE("logout and disconnect, while loading or in the world, leave nothing of the world",
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

// What a character leaves behind when another logs in: nothing.
//
// core::CharacterSession owns, per character in the world, the interface's
// own state (addons::InterfaceState) and the Lua state its bindings run in.
// Each character's Lua state is bound to that character's InterfaceState; a
// /reload builds a new Lua state over the same one.
//
// The bindings driven here are the real LFG ones (registerLfgLuaAPI): which
// dungeons are ticked and which headers folded were function statics, so the
// next character opened the dungeon finder with the last one's choices made.
// The others moved the same way and are held there by
// tools/function_static_check.py.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "addons/interface_state.hpp"
#include "addons/interface_state_binding.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/toc_parser.hpp"
#include "core/character_identity.hpp"
#include "game/opcodes.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <string>

using wowee::addons::InterfaceState;

namespace {

/// A Lua state as the engine builds one for a character: the LFG bindings,
/// the game, and that character's interface state (none for `nullptr`).
struct CharacterLua {
    lua_State* L = luaL_newstate();

    CharacterLua(wowee::game::GameHandler& game, InterfaceState* state) {
        lua_pushcfunction(L, luaopen_base);
        lua_pushstring(L, "");
        lua_call(L, 1, 0);
        lua_pushlightuserdata(L, &game);
        lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
        if (state) wowee::addons::bindInterfaceState(L, state);
        wowee::addons::registerLfgLuaAPI(L);
    }
    ~CharacterLua() { lua_close(L); }
    CharacterLua(const CharacterLua&) = delete;
    CharacterLua& operator=(const CharacterLua&) = delete;

    void run(const char* code) {
        INFO(code);
        if (luaL_dostring(L, code) != 0) {
            const char* err = lua_tostring(L, -1);
            FAIL((err ? err : "?"));
        }
    }
    bool truthy(const char* expression) {
        const std::string code = std::string("return ") + expression;
        INFO(code);
        REQUIRE(luaL_dostring(L, code.c_str()) == 0);
        const bool out = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return out;
    }
};

}  // namespace

TEST_CASE("the next character's dungeon finder starts with nothing the last one chose",
          "[charactersession]") {
    wowee::test::GameHarness h;
    h.enterWorld();

    // Character A ticks a dungeon and folds a header.
    auto stateA = std::make_unique<InterfaceState>();
    {
        CharacterLua a(h.game(), stateA.get());
        a.run("SetLFGDungeonEnabled(285, true); SetLFGHeaderCollapsed(261, true)");
        CHECK(a.truthy("GetLFDChoiceEnabledState()[285]"));
        CHECK(a.truthy("GetLFDChoiceCollapseState()[261]"));
    }
    // A logs out: its session - Lua state and interface state - ends.
    stateA.reset();

    // B logs in on the same client and gets a session of its own.
    InterfaceState stateB;
    CharacterLua b(h.game(), &stateB);
    CHECK(b.truthy("next(GetLFDChoiceEnabledState()) == nil"));
    CHECK(b.truthy("next(GetLFDChoiceCollapseState()) == nil"));
}

TEST_CASE("a reload keeps the character's dungeon finder choices", "[charactersession]") {
    wowee::test::GameHarness h;
    h.enterWorld();
    // /reload: a new Lua state over the same character's interface state.
    InterfaceState state;
    {
        CharacterLua before(h.game(), &state);
        before.run("SetLFGDungeon(285)");
    }
    CharacterLua after(h.game(), &state);
    CHECK(after.truthy("GetLFDChoiceEnabledState()[285]"));
}

TEST_CASE("a Lua state with no interface state refuses rather than sharing one",
          "[charactersession]") {
    wowee::test::GameHarness h;
    h.enterWorld();
    CharacterLua unbound(h.game(), nullptr);
    REQUIRE(luaL_dostring(unbound.L, "SetLFGDungeonEnabled(285, true)") != 0);
    const char* err = lua_tostring(unbound.L, -1);
    CHECK(std::string(err ? err : "").find("no interface state") != std::string::npos);
}

TEST_CASE("a relog's session is the new character's, and so are its saved variables",
          "[charactersession][harness]") {
    using wowee::game::Opcode;
    using wowee::test::GameHarness;
    using wowee::test::PlayerSpec;

    PlayerSpec alice;
    alice.guid = 0x21;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x22;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    const auto first = wowee::core::characterIdentityOf(
        h.game().getPlayerGuid(), h.game().getCharacters(),
        h.game().lookupName(h.game().getPlayerGuid()));
    CHECK(first.guid == alice.guid);
    CHECK(first.name == "Alice");

    // Logout to the character list, then the other character in.
    h.game().requestLogout(false);
    h.receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.pump();
    h.game().returnToCharacterSelect();
    REQUIRE_FALSE(h.game().isInWorld());
    h.receive(Opcode::SMSG_CHAR_ENUM, GameHarness::charEnumBody({alice, bob}));
    h.pump();
    h.game().selectCharacter(bob.guid);
    REQUIRE(h.lastSent(Opcode::CMSG_PLAYER_LOGIN));
    h.receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, GameHarness::loginVerifyWorldBody(bob));
    h.pump();
    REQUIRE(h.game().isInWorld());

    const auto second = wowee::core::characterIdentityOf(
        h.game().getPlayerGuid(), h.game().getCharacters(),
        h.game().lookupName(h.game().getPlayerGuid()));
    CHECK(second.guid == bob.guid);
    CHECK(second.name == "Bob");

    // Bob's interface reads and writes Bob's file, never Alice's.
    wowee::addons::TocFile addon;
    addon.addonName = "Bagnon";
    addon.basePath = "AddOns/Bagnon";
    CHECK(addon.savedVariablesPerCharacterPath(second.name) !=
          addon.savedVariablesPerCharacterPath(first.name));
}

TEST_CASE("a character nobody can name writes no per-character saved variables",
          "[charactersession]") {
    // Not in the list and not in the name cache: the session has no name,
    // and an empty path is what makes the interface skip the file rather than
    // write one that belongs to nobody - or to whoever was named last.
    std::vector<wowee::game::Character> list(1);
    list[0].guid = 7;
    list[0].name = "Listed";
    const auto unnamed = wowee::core::characterIdentityOf(8, list, "");
    wowee::addons::TocFile addon;
    addon.addonName = "Bagnon";
    addon.basePath = "AddOns/Bagnon";
    CHECK(addon.savedVariablesPerCharacterPath(unnamed.name).empty());
    // The name cache answers for a guid the list does not have.
    CHECK(wowee::core::characterIdentityOf(8, list, "Cached").name == "Cached");
    CHECK(wowee::core::characterIdentityOf(7, list, "Cached").name == "Listed");
}

#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "test_support.hpp"
#include "addons/lua_event_args.hpp"
#include "addons/lua_handler_globals.hpp"
#include "addons/lua_services.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

using wowee::test::GameHarness;
using wowee::test::Body;
using wowee::game::Opcode;

namespace {
std::vector<uint8_t> startCast(uint64_t player, uint32_t spell) {
    return Body().packedGuid(player).packedGuid(player).u8(1).u32(spell)
        .u32(0).u32(3000).u32(0);
}

int castingInfo(lua_State* L) {
    const auto& game = *static_cast<wowee::game::GameHandler*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!game.isCasting()) return 0;
    lua_pushstring(L, "Spell");
    lua_pushstring(L, "");
    lua_pushstring(L, "Spell");
    lua_pushstring(L, "icon");
    lua_pushnumber(L, 0);
    lua_pushnumber(L, game.getCastTimeTotal() * 1000);
    lua_pushboolean(L, false);
    lua_pushnumber(L, game.getCurrentCastSpellId());
    lua_pushboolean(L, false);
    return 9;
}

struct LuaState {
    lua_State* L = luaL_newstate();
    ~LuaState() { lua_close(L); }
};
}

TEST_CASE("queued notifications preserve FIFO across reentrant publication", "[addonqueue][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<std::string> delivered;
    h.game().setAddonEventCallback([&](const auto& event, const auto&) {
        delivered.push_back(event);
        if (event == "FIRST") h.game().fireAddonEvent("THIRD");
    });
    h.game().setInterfaceCommandCallback([&](const auto& code) { delivered.push_back(code); });
    h.game().fireAddonEvent("FIRST");
    std::string command = "COMMAND";
    h.game().runInterfaceCommand(command);
    command = "changed";
    h.game().fireAddonEvent("SECOND");
    h.game().updateNetwork(0);
    h.game().updateGame(0);
    CHECK(delivered.empty());
    h.game().dispatchAddonEvents();
    CHECK(delivered == std::vector<std::string>{"FIRST", "COMMAND", "SECOND"});
    h.game().dispatchAddonEvents();
    CHECK(delivered == std::vector<std::string>{"FIRST", "COMMAND", "SECOND", "THIRD"});
}

TEST_CASE("FrameXML castbar survives queued starts failures and replacement casts", "[addonqueue][harness][framexml]") {
    const auto script = wowee::test::sourceRoot() + "Data/expansions/wotlk/interface/framexml/castingbarframe.lua";
    if (!std::filesystem::exists(script)) SKIP("Extracted WotLK FrameXML is required for the castbar integration test");
    GameHarness h;
    h.enterWorld();
    LuaState lua;
    auto* L = lua.L;
    lua_pushcfunction(L, luaopen_base);
    lua_pushstring(L, "");
    lua_call(L, 1, 0);
    auto phase = wowee::core::Phase::Interface;
    wowee::addons::LuaServices services;
    services.executionPhase = [&] { return phase; };
    lua_pushlightuserdata(L, &services);
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_lua_services");
    lua_pushlightuserdata(L, &h.game());
    lua_pushcclosure(L, castingInfo, 1);
    lua_setglobal(L, "UnitCastingInfo");
    REQUIRE(luaL_loadfile(L, script.c_str()) == 0);
    REQUIRE(wowee::addons::checkedLuaCall(L, 0, 0, 0) == 0);
    REQUIRE(luaL_loadstring(L, R"lua(
        function GetTime() return 0 end
        bar = {unit = 'player', showCastbar = true, showTradeSkills = true}
        function bar:GetName() return 'QueueTestBar' end
        function bar:Hide() self.shown = false end
        function bar:Show() self.shown = true end
        function bar:IsShown() return self.shown end
        bar.IsVisible = bar.IsShown
        function bar:SetStatusBarColor() end
        function bar:SetMinMaxValues() end
        function bar:SetValue() end
        function bar:SetAlpha() end
    )lua") == 0);
    REQUIRE(wowee::addons::checkedLuaCall(L, 0, 0, 0) == 0);
    std::vector<std::string> delivered;
    std::vector<bool> stateAtStart;
    h.game().setAddonEventCallback([&](const auto& event, const auto& args) {
        REQUIRE(phase == wowee::core::Phase::Interface);
        if (event.rfind("UNIT_SPELLCAST_", 0) != 0) return;
        delivered.push_back(event);
        if (event == "UNIT_SPELLCAST_START") stateAtStart.push_back(h.game().isCasting());
        lua_getglobal(L, "CastingBarFrame_OnEvent");
        lua_getglobal(L, "bar");
        lua_pushstring(L, event.c_str());
        for (const auto& arg : args) wowee::addons::pushEventArg(L, arg);
        const int rc = wowee::addons::pcallScript(L, "OnEvent", static_cast<int>(args.size()) + 2, 0);
        INFO((rc == 0 ? "OK" : lua_tostring(L, -1)));
        REQUIRE(rc == 0);
    });
    const auto failure = [](uint32_t spell) { return Body().u8(1).u32(spell).u8(12).bytes(); };
    const auto network = [&] {
        phase = wowee::core::Phase::Network;
        h.game().updateNetwork(0);
        phase = wowee::core::Phase::Game;
        h.game().updateGame(0);
    };
    const auto interface = [&] {
        phase = wowee::core::Phase::Interface;
        h.game().dispatchAddonEvents();
    };
    const auto barState = [&](const char* expression) {
        const std::string chunk = std::string("return ") + expression;
        REQUIRE(luaL_loadstring(L, chunk.c_str()) == 0);
        REQUIRE(wowee::addons::checkedLuaCall(L, 0, 1, 0) == 0);
        const bool result = lua_toboolean(L, -1);
        lua_pop(L, 1);
        return result;
    };

    h.receive(Opcode::SMSG_SPELL_START, startCast(h.player().guid, 133));
    h.receive(Opcode::SMSG_CAST_FAILED, failure(133));
    network();
    CHECK(delivered.empty());
    CHECK_FALSE(h.game().isCasting());
    interface();
    CHECK(stateAtStart == std::vector<bool>{false}); // Event sees the newer game state.
    CHECK(delivered == std::vector<std::string>{"UNIT_SPELLCAST_START", "UNIT_SPELLCAST_INTERRUPTIBLE",
                                              "UNIT_SPELLCAST_FAILED", "UNIT_SPELLCAST_STOP"});
    CHECK(barState("not bar.shown and not bar.casting"));

    h.receive(Opcode::SMSG_SPELL_START, startCast(h.player().guid, 133));
    h.receive(Opcode::SMSG_CAST_FAILED, failure(133));
    h.receive(Opcode::SMSG_SPELL_START, startCast(h.player().guid, 116));
    network();
    interface();
    CHECK(barState("bar.shown and bar.casting and bar.castID == 116"));

    h.receive(Opcode::SMSG_CAST_FAILED, failure(116));
    network();
    interface();
    CHECK(barState("not bar.casting and bar.fadeOut"));
}

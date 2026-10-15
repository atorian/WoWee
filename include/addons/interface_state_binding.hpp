#pragma once

// How a Lua state's bindings find the InterfaceState they work on.
//
// The pointer is kept in that state's registry, so it lives exactly as long as
// the state does: nothing outside the Lua state holds it, and a state built for
// the next character is handed that character's InterfaceState. Header-only
// and dependent on nothing but Lua, so the rule can be tested without the
// interface (tests/test_character_session.cpp).

#include "addons/interface_state.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace wowee::addons {

inline constexpr const char* kInterfaceStateRegistryKey = "wowee_interface_state";

/// Hand a Lua state the interface state its bindings read and write. The
/// state must outlive the Lua state, which CharacterSession's member order
/// guarantees.
inline void bindInterfaceState(lua_State* L, InterfaceState* state) {
    lua_pushlightuserdata(L, state);
    lua_setfield(L, LUA_REGISTRYINDEX, kInterfaceStateRegistryKey);
}

/// The interface state of the character whose interface this Lua state is.
///
/// Every Lua state the client builds is bound before a binding can run, so an
/// unbound one is a fault in whoever built it. It is raised as a Lua error -
/// the bindings run inside a protected call - rather than answered from some
/// state shared between characters, which is the fault this exists to remove.
/// The engine reports a Lua error with its traceback, once per message.
inline InterfaceState& interfaceState(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kInterfaceStateRegistryKey);
    auto* state = static_cast<InterfaceState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!state) luaL_error(L, "no interface state is bound to this Lua state");
    return *state;
}

} // namespace wowee::addons

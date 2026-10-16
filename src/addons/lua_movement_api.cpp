// lua_movement_api.cpp - the movement functions bindings.xml calls.
//
// The original client's key bindings move the character through these:
// MOVEFORWARD runs MoveForwardStart() on the press and MoveForwardStop() on the
// release, JUMP runs JumpOrAscendStart() and AscendStop(), and so on. Each is
// handed to the client as a command held or let go, which is what lets a key
// the account binds elsewhere stop moving the character and a key it binds
// here start.

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/lua_services.hpp"
#include "core/logger.hpp"
#include "rendering/movement_command.hpp"

namespace wowee::addons {

namespace {

using rendering::MovementCommand;

/// Hold or let go of a command, through the client.
void holdMovement(lua_State* L, MovementCommand command, bool held) {
    auto* svc = getLuaServices(L);
    if (svc && svc->holdMovement) {
        svc->holdMovement(command, held);
        return;
    }
    // Once: a binding script calls these on every press and release.
    static bool said = false;
    if (!said) {
        said = true;
        LOG_WARNING("Movement binding (", rendering::movementCommandName(command),
                    "): this client has nothing to move; the key does nothing");
    }
}

template <MovementCommand Command, bool Held>
int lua_hold(lua_State* L) {
    holdMovement(L, Command, Held);
    return 0;
}

int lua_ToggleAutoRun(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (svc && svc->toggleAutoRun) {
        svc->toggleAutoRun();
    } else {
        LOG_WARNING("ToggleAutoRun: this client has nothing to move; autorun unchanged");
    }
    return 0;
}

int lua_ToggleRun(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (svc && svc->toggleRun) {
        svc->toggleRun();
    } else {
        LOG_WARNING("ToggleRun: this client has nothing to move; walking unchanged");
    }
    return 0;
}

} // namespace

void registerMovementLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
        {"MoveForwardStart",       lua_hold<MovementCommand::Forward, true>},
        {"MoveForwardStop",        lua_hold<MovementCommand::Forward, false>},
        {"MoveBackwardStart",      lua_hold<MovementCommand::Backward, true>},
        {"MoveBackwardStop",       lua_hold<MovementCommand::Backward, false>},
        {"TurnLeftStart",          lua_hold<MovementCommand::TurnLeft, true>},
        {"TurnLeftStop",           lua_hold<MovementCommand::TurnLeft, false>},
        {"TurnRightStart",         lua_hold<MovementCommand::TurnRight, true>},
        {"TurnRightStop",          lua_hold<MovementCommand::TurnRight, false>},
        {"StrafeLeftStart",        lua_hold<MovementCommand::StrafeLeft, true>},
        {"StrafeLeftStop",         lua_hold<MovementCommand::StrafeLeft, false>},
        {"StrafeRightStart",       lua_hold<MovementCommand::StrafeRight, true>},
        {"StrafeRightStop",        lua_hold<MovementCommand::StrafeRight, false>},
        {"JumpOrAscendStart",      lua_hold<MovementCommand::JumpOrAscend, true>},
        {"AscendStop",             lua_hold<MovementCommand::JumpOrAscend, false>},
        {"SitStandOrDescendStart", lua_hold<MovementCommand::SitOrDescend, true>},
        {"DescendStop",            lua_hold<MovementCommand::SitOrDescend, false>},
        {"ToggleAutoRun",          lua_ToggleAutoRun},
        {"ToggleRun",              lua_ToggleRun},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons

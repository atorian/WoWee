#include "addons/lua_event_args.hpp"
#include "game/handler_types.hpp"
extern "C" {
#include <lua.h>
}
namespace wowee::addons {
/// Pushes an event argument with the type WoW gives it.
///
/// Every argument crosses this boundary as a string, and some of them are
/// numbers on the other side: ChatFrame_MessageEventHandler does
/// `if (arg8 > 0)`, and comparing a string with a number is not false in Lua,
/// it is an error that takes the handler down - which for chat is every
/// message. Only a plain integer converts, so a name, a unit id and a hex guid
/// all stay strings.
void pushEventArg(lua_State* L, const std::string& arg) {
    // The one argument that is not text: see kEventNil. A false boolean has no
    // spelling as a string, and Lua treats every string and every number as
    // true, so without this an event could not carry one.
    if (arg == wowee::game::kEventNil) { lua_pushnil(L); return; }
    if (!arg.empty() && arg.size() < 12) {
        size_t at = (arg[0] == '-') ? 1 : 0;
        if (at < arg.size()) {
            bool digits = true;
            // One decimal point is still a number. UPDATE_TICKET carries ages
            // and wait times measured in days, all of them fractions of one,
            // and the help frame compares them against zero - as a string that
            // is the same error this exists to avoid, not a wrong answer.
            int points = 0;
            for (size_t i = at; i < arg.size(); ++i) {
                if (arg[i] == '.' && points == 0 && i != at && i + 1 < arg.size()) {
                    ++points;
                    continue;
                }
                if (arg[i] < '0' || arg[i] > '9') { digits = false; break; }
            }
            // "007" is not a number anyone meant; a leading zero is a string,
            // except the one in front of a decimal point.
            const bool canonical = digits &&
                (arg.size() - at == 1 || arg[at] != '0' ||
                 (points == 1 && arg[at + 1] == '.'));
            if (canonical) {
                lua_pushnumber(L, std::stod(arg));
                return;
            }
        }
    }
    lua_pushstring(L, arg.c_str());
}

}

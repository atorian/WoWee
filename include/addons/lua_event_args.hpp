#pragma once
#include <string>
struct lua_State;
namespace wowee::addons {
/// Convert the game notification payload to the FrameXML event contract.
void pushEventArg(lua_State* L, const std::string& arg);
}

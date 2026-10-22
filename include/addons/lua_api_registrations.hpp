// lua_api_registrations.hpp - Forward declarations for per-domain Lua API
// registration functions.  Called from LuaEngine::registerCoreAPI().
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#pragma once

#include <string>

struct lua_State;

namespace wowee::core { class CVarStore; }

namespace wowee::addons {

void registerUnitLuaAPI(lua_State* L);
void registerSpellLuaAPI(lua_State* L);
void registerInventoryLuaAPI(lua_State* L);
void registerQuestLuaAPI(lua_State* L);
void registerLfgLuaAPI(lua_State* L);
void registerSocketLuaAPI(lua_State* L);
void registerSocialLuaAPI(lua_State* L);
void registerSystemLuaAPI(lua_State* L);

/// Re-apply every CVar restored from disk, now that the services behind them
/// exist. See applyCVarSideEffects.
void applyStoredCVarSideEffects(lua_State* L);

/// Re-apply the sound switches from the cvar store.
///
/// The settings panel owns those switches now, and writing a setting writes
/// the cvar behind it - but nothing was re-reading them, so the switch moved
/// and the sound did not change until something else called SetCVar.
void applySoundCVarSideEffects(lua_State* L);

/// The client's CVar store, which every CVar binding here reads and writes.
/// Handed over once, as the client starts and before anything reads a CVar.
void useCVarStore(core::CVarStore& store);

/// One CVar's saved value.
///
/// For the settings that have to be known before the Lua state exists - the
/// shadow map's size is chosen when the per-frame resources are built, which
/// is long before the interface loads. Returns the fallback when nothing set
/// it, so a fresh install gets the default rather than nothing.
std::string storedCVarValue(const std::string& key, const std::string& fallback);

/// Tell the CVar store that a setting it drives has been changed elsewhere.
///
/// Six settings are driven by a Blizzard control through kClientCVars, and
/// three of those also have a control in this client's own settings window.
/// The store is applied over the settings file at start-up - it is the record
/// of what the player last set - so a change made in this client's window and
/// saved to settings.cfg was undone at the next start by a CVar nobody had
/// touched since.
///
/// Does nothing for a setting no CVar drives, and writes only when the value
/// actually moves: the settings window applies on every frame of a slider drag,
/// and saving the store means writing a file.
void noteClientSettingChanged(const std::string& settingKey, const std::string& value);

/// Put a value in the CVar store and on disk, without the CVAR_UPDATE the
/// interface's own SetCVar announces.
///
/// For the things the player sets by using the interface rather than by
/// setting a setting - the minimap's zoom is the first - which have to be
/// there again on the next start. The store is written on every change for the
/// reason its own file says: a value that survives only a clean exit is not one
/// a player can rely on, and this client is exited every way there is.
void setStoredCVar(const std::string& key, const std::string& value);
void registerActionLuaAPI(lua_State* L);
void refreshMacroIcons(lua_State* L, float elapsed);
/// MoveForwardStart and the rest of what bindings.xml moves the character
/// with, through LuaServices::holdMovement.
void registerMovementLuaAPI(lua_State* L);

/// Run a binding command's press or its release, as a key bound to it does
/// and as RunBinding(command, keystate) asks.
///
/// A command is a bindings.xml script, given keystate "down" or "up" - or one
/// of the four the original client writes for a key bound straight to
/// something: "SPELL name", "MACRO name", "ITEM name" and "CLICK frame:button",
/// from SetBindingSpell, SetBindingMacro, SetBindingItem and SetBindingClick.
/// Those four act on the press. Says so in the log, once per command, when
/// nothing in the interface runs it; false then, and when it raised.
bool runBindingCommand(lua_State* L, const std::string& command, bool down);

/// Whether bindings.xml marks a command runOnUp - its script is run for the
/// release as well as the press.
bool bindingRunsOnUp(lua_State* L, const std::string& command);

/// Whether this client performs a binding command itself, without the
/// interface.
///
/// A key answered by both is answered twice, and most bindings toggle
/// something - so twice means the panel opens and shuts again on one press and
/// the key reads as dead. Consulted before running a binding.
bool clientActsOnBinding(const std::string& command);

/// The ImGui key the client listens on for a binding command, when it is one
/// of the panels KeybindingManager answers; 0 (ImGuiKey_None) otherwise.
///
/// A controller button bound to one of those has to press that key, since the
/// manager asks ImGui rather than the binding table.
int clientImGuiKeyForBinding(const std::string& command);

} // namespace wowee::addons

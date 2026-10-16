// lua_action_api.cpp - Action bar, cursor/pickup, keyboard input, key bindings, and pet actions Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/item_text.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
// For fireEvent: paging has to reach the frames as well as the addons.
#include "addons/lua_engine.hpp"
#include "addons/lua_handler_globals.hpp"
#include "game/inventory_slots.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/gamepad_controls.hpp"

#include <span>
#include "core/config_paths.hpp"
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <cstring>
#include "game/pet_action.hpp"
#include "game/account_data_handler.hpp"
#include "imgui.h"
#include <optional>
#include <SDL3/SDL_keyboard.h>

namespace wowee::addons {

// What the interface is holding.
//
// This is the original interface's cursor, and it is not the only one: this
// client's inventory screen keeps a held item of its own. They do not need
// merging, because whichever interface is drawing the bags is the one the
// player can pick anything up from, and that is the cursor in use - with the
// bags owned, as they are by default, this is it.
//
// What that does mean is that CursorHasItem answers for this cursor alone. An
// addon asking while the player is dragging in this client's own bags is told
// no, which is true of the cursor it can see and not of the player's hand.
// MERCHANT is a fifth kind and it is not an item: what the cursor holds is a
// vendor's list index, not something the player owns. FrameXML's merchant
// window puts one there on every left-click - PickupMerchantItem - and buying
// is what happens when it is dropped into a bag, so without it a left-click at
// a vendor did nothing at all and only right-click bought.
//
// The cursor is the character's: InterfaceState::Cursor, reached through the
// Lua state. A guild bank split rides on it between the pickup and the drop,
// because the wire carries the split on the *withdrawal*.
static InterfaceState::Cursor& cursor(lua_State* L) { return interfaceState(L).cursor; }
static void setCursorType(lua_State* L, CursorType type);
/// Where the cursor's item came from, in wire bag/slot space. Declared here
/// because the guild bank pickup above needs it and it is defined further down
/// with the other cursor helpers.
static bool cursorWireSlot(lua_State* L, uint8_t& bag, uint8_t& slot);
/// Whether a drop on an inventory slot is a move at all, and puts the cursor
/// down when it is not. See kCursorNoSource: an item off the action bar has
/// nowhere to be moved from, so dropping it in a bag or on the paperdoll does
/// nothing but end the drag, which is what the real client does.
static bool droppedItemFromNowhere(lua_State* L);
/// Sell what the cursor is holding. Defined below, beside the buy it mirrors.
static void soldHeldItem(lua_State* L);

void setCursorMoney(lua_State* L, uint64_t copper) {
    // Through setCursorType rather than by assignment: it is what fires
    // CURSOR_UPDATE and what shows the action bars' empty slots. Setting the
    // type by hand would put money on the cursor with nothing told about it.
    cursor(L).money = copper;
    if (copper == 0) {
        if (cursor(L).type == CursorType::MONEY) setCursorType(L, CursorType::NONE);
        return;
    }
    cursor(L).id = 0;
    cursor(L).slot = 0;
    cursor(L).bag = -1;
    setCursorType(L, CursorType::MONEY);
}

// --- Macros ---
//
// The interface's number for a macro is its place in the macro frame: the
// account's 1-36 and the character's 37-54, each half listed by name. The
// account data keeps an id instead, which is what an action button holds.

static constexpr const char* kQuestionMarkIcon = "Interface\\Icons\\INV_Misc_QuestionMark";

/// The macro at a one-based place in the frame, or nullptr.
static const game::Macro* macroAtIndex(game::GameHandler* gh, int index) {
    if (!gh || index < 1 || index > game::kMaxAccountMacros + game::kMaxCharacterMacros) return nullptr;
    const bool character = index > game::kMaxAccountMacros;
    const auto& half = gh->accountData().macros(character);
    const size_t pos = static_cast<size_t>(index - 1 - (character ? game::kMaxAccountMacros : 0));
    return pos < half.size() ? &half[pos] : nullptr;
}

/// A macro's place in the frame, or 0 when it is not one.
static int macroIndexOf(game::GameHandler* gh, uint32_t id) {
    for (int c = 0; gh && c < 2; ++c) {
        const auto& half = gh->accountData().macros(c == 1);
        for (size_t i = 0; i < half.size(); ++i) {
            if (half[i].id == id) return static_cast<int>(i) + 1 + (c == 1 ? game::kMaxAccountMacros : 0);
        }
    }
    return 0;
}

/// The macro an argument names: a place in the frame, or a name - which is
/// how a macro written inside another names it.
static const game::Macro* macroFromArg(lua_State* L, int arg) {
    auto* gh = getGameHandler(L);
    if (!gh) return nullptr;
    if (lua_type(L, arg) == LUA_TNUMBER) {
        return macroAtIndex(gh, static_cast<int>(lua_tonumber(L, arg)));
    }
    const char* name = lua_tostring(L, arg);
    if (!name) return nullptr;
    for (int c = 0; c < 2; ++c) {
        for (const auto& macro : gh->accountData().macros(c == 1)) {
            if (macro.name == name) return &macro;
        }
    }
    return nullptr;
}

/// The texture an icon name is drawn from.
static std::string macroTexture(const game::Macro& macro) {
    return macro.icon.empty() ? std::string(kQuestionMarkIcon) : "Interface\\Icons\\" + macro.icon;
}

/// An icon argument as a texture path: the macro frame passes its picker's
/// position in GetMacroIconInfo's list, an addon a path.
static std::string macroIconArg(lua_State* L, int arg) {
    if (lua_type(L, arg) == LUA_TNUMBER) {
        lua_getglobal(L, "GetMacroIconInfo");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return {}; }
        lua_pushvalue(L, arg);
        if (lua_pcall(L, 1, 1, 0) != 0) {
            LOG_WARNING("GetMacroIconInfo(", lua_tonumber(L, arg), ") failed: ",
                        lua_tostring(L, -1) ? lua_tostring(L, -1) : "?", "; the macro keeps no icon");
            lua_pop(L, 1);
            return {};
        }
        const char* path = lua_tostring(L, -1);
        std::string out = path ? path : "";
        lua_pop(L, 1);
        return out;
    }
    const char* path = lua_tostring(L, arg);
    return path ? path : "";
}

/// PickupPetAction(slot) - pick a pet ability up off the pet bar.
///
/// Defined and does nothing. The cursor here holds spells, items, actions and
/// macros; a pet action is a fifth kind in its own slot space, and moving one
/// is a server operation this client does not send. Inventing a cursor state
/// that cannot be put down anywhere would be worse than not picking up.
///
/// Defined because it is reachable three ways - shift-clicking a pet button,
/// dragging one, and dropping on one - and the pet bar is drawn. Undefined,
/// the first shift-click on a pet ability takes the bar down. Clicking a pet
/// ability to use it goes through a different path and works; only rearranging
/// the bar is lost.
static int lua_PickupPetAction(lua_State* L) { (void)L; return 0; }

/// Change what the cursor is holding, and say so.
///
/// CURSOR_UPDATE is how the interface learns the cursor picked something up or
/// put it down - action buttons redraw their highlight on it, and the
/// equipment and container frames test the cursor from it. Thirteen places set
/// this and none of them said anything, so the cursor changed silently.
///
/// Every one of those thirteen goes through here. Converting some and not
/// others would give a cursor that announces half its changes, which is harder
/// to reason about than one that announces none.
///
/// Only on an actual change: several of these run on every click of an empty
/// slot, setting NONE over NONE, and the interface would redraw for each.
/// Defined below, beside the rest of the cursor state it clears. Declared here
/// because the sites that put the cursor down come first in the file, and each
/// of them used to clear only part of what a pickup sets.
static void clearCursorItem(lua_State* L);

static void setCursorType(lua_State* L, CursorType type) {
    if (cursor(L).type == type) return;
    // Putting anything down forgets the guild bank split. It rides on the
    // cursor between the pickup and the drop, and a leftover would be applied
    // to the next withdrawal - a stack the player never asked to divide.
    if (type != CursorType::GUILDBANK) cursor(L).split = 0;
    const bool wasCarrying = (cursor(L).type != CursorType::NONE);
    const bool nowCarrying = (type != CursorType::NONE);
    cursor(L).type = type;
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!engine) return;
    engine->fireEvent("CURSOR_UPDATE", {});
    // Picking something up shows the empty slots of the action bars, so there
    // is somewhere visible to drop it; putting it down hides them again. Every
    // action button registers for both and neither was ever fired, so dragging
    // a spell towards the bar showed no targets at all - the bar looked as
    // though it would not take it.
    //
    // Fired on the change rather than on every set: this function is called
    // with the same type repeatedly and the grid would flicker.
    if (nowCarrying && !wasCarrying) {
        engine->fireEvent("ACTIONBAR_SHOWGRID", {});
    } else if (wasCarrying && !nowCarrying) {
        engine->fireEvent("ACTIONBAR_HIDEGRID", {});
    }
}

static int lua_HasAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1; // WoW uses 1-indexed slots
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size())) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, !bar[slot].isEmpty());
    return 1;
}

// GetActionTexture(slot) → texturePath or nil
static int lua_GetActionTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::SPELL) {
        std::string icon = gh->getSpellIconPath(action.id);
        if (!icon.empty()) {
            lua_pushstring(L, icon.c_str());
            return 1;
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        const auto* info = gh->getItemInfo(action.id);
        if (info && info->displayInfoId != 0) {
            std::string icon = gh->getItemIconPath(info->displayInfoId);
            if (!icon.empty()) {
                lua_pushstring(L, icon.c_str());
                return 1;
            }
        }
    } else if (action.type == game::ActionBarSlot::MACRO) {
        // The macro's own icon. Without this branch every macro button was
        // drawn as an empty slot with only its name under it.
        if (const auto* macro = gh->accountData().macro(action.id)) {
            lua_pushstring(L, macroTexture(*macro).c_str());
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// IsCurrentAction(slot) → boolean
static int lua_IsCurrentAction(lua_State* L) {
    // Currently no "active action" tracking; return false
    (void)L;
    lua_pushboolean(L, 0);
    return 1;
}

// IsUsableAction(slot) → usable, notEnoughMana
static int lua_IsUsableAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    const auto& action = bar[slot];
    // Not the cooldown. WoW's answer here is about whether the action *could*
    // be used - known, affordable, the right stance - and a spell waiting on a
    // cooldown is all of those. The wait is shown by the sweep over the icon,
    // which is drawn from GetActionCooldown and nothing to do with this.
    //
    // Folding it in here painted the icon dark grey for the whole of every
    // cooldown, because ActionButton_UpdateUsable has three branches and
    // "not usable, mana is fine" is the one that means unusable.
    bool usable = true;
    bool noMana = false;
    if (action.type == game::ActionBarSlot::SPELL) {
        usable = gh->getKnownSpells().count(action.id) > 0;
        // Check power cost
        if (usable && action.id != 0) {
            auto spellData = gh->getSpellData(action.id);
            if (spellData.manaCost > 0) {
                auto pe = gh->getEntityManager().getEntity(gh->getPlayerGuid());
                if (pe) {
                    auto* unit = dynamic_cast<game::Unit*>(pe.get());
                    if (unit && unit->getPower() < spellData.manaCost) {
                        noMana = true;
                        usable = false;
                    }
                }
            }
        }
    }
    lua_pushboolean(L, usable ? 1 : 0);
    lua_pushboolean(L, noMana ? 1 : 0);
    return 2;
}

// IsActionInRange(slot) → 1 if in range, 0 if out, nil if no range check applicable
static int lua_IsActionInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = bar[slot];
    uint32_t spellId = 0;
    if (action.type == game::ActionBarSlot::SPELL) {
        spellId = action.id;
    } else {
        // Items/macros: no range check for now
        lua_pushnil(L);
        return 1;
    }
    return pushSpellRangeAnswer(L, gh, spellId, gh->getTargetGuid());
}

// GetActionInfo(slot) → actionType, id, subType
static int lua_GetActionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return 0; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        return 0;
    }
    const auto& action = bar[slot];
    switch (action.type) {
        case game::ActionBarSlot::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "spell");
            // The fourth value, which for a spell action is the same id.
            // vehiclemenubar reads it to build a link on a modified click -
            // HandleModifiedItemClick(GetSpellLink(spellID)) - so without it
            // shift-clicking a vehicle button linked nothing. Only spells have
            // one; an item or a macro answers nil, as the real client does.
            lua_pushnumber(L, action.id);
            return 4;
        case game::ActionBarSlot::ITEM:
            lua_pushstring(L, "item");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "item");
            return 3;
        case game::ActionBarSlot::MACRO:
            // The macro's place in the macro frame, which is the number the
            // interface hands back to GetMacroInfo - not the id the slot
            // holds.
            lua_pushstring(L, "macro");
            lua_pushnumber(L, macroIndexOf(gh, action.id));
            lua_pushstring(L, "macro");
            return 3;
        default:
            return 0;
    }
}

// GetActionCount(slot) → count (item stack count or 0)
static int lua_GetActionCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        lua_pushnumber(L, gh->getInventory().countItem(action.id));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetActionCooldown(slot) → start, duration, enable
static int lua_GetActionCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
        return 3;
    }
    const auto& action = bar[slot];
    if (action.cooldownRemaining > 0.0f) {
        // WoW returns GetTime()-based start time; approximate
        double now = 0;
        lua_getglobal(L, "GetTime");
        if (lua_isfunction(L, -1)) {
            lua_call(L, 0, 1);
            now = lua_tonumber(L, -1);
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
        }
        double start = now - (action.cooldownTotal - action.cooldownRemaining);
        lua_pushnumber(L, start);
        lua_pushnumber(L, action.cooldownTotal);
        lua_pushnumber(L, 1);
    } else if (action.type == game::ActionBarSlot::SPELL && gh->isGCDActive()) {
        // No individual cooldown but GCD is active - show GCD sweep
        float gcdRem = gh->getGCDRemaining();
        float gcdTotal = gh->getGCDTotal();
        double now = 0;
        lua_getglobal(L, "GetTime");
        if (lua_isfunction(L, -1)) { lua_call(L, 0, 1); now = lua_tonumber(L, -1); lua_pop(L, 1); }
        else lua_pop(L, 1);
        double elapsed = gcdTotal - gcdRem;
        lua_pushnumber(L, now - elapsed);
        lua_pushnumber(L, gcdTotal);
        lua_pushnumber(L, 1);
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
    }
    return 3;
}

// UseAction(slot, unit, button) - activate action bar slot (1-indexed)
//
// The second argument is a unit, and it was ignored. securetemplates is the
// only caller and it passes what the button's own attributes resolved to:
// nil for an ordinary click, and "player" when the self-cast modifier is
// held - which is the whole of how self-casting works. Dropping it meant
// every action went at the current target, so self-cast did nothing, and a
// unit-frame button carrying its own unit acted on whoever was targeted
// instead.
static int lua_UseAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) return 0;
    const auto& action = bar[slot];
    // The end of the chain a click travels, and the last place it can stop
    // without saying so: an empty slot, a cooldown, or a type nothing acts on
    // all look the same from the button. At debug level, because pressing a
    // button on the action bar is the most ordinary thing a player does and
    // this was a warning on every one of them.
    LOG_DEBUG("UseAction: slot ", slot + 1, " type=", static_cast<int>(action.type),
              " id=", action.id, " ready=", action.isReady() ? 1 : 0);
    if (action.type == game::ActionBarSlot::SPELL && action.isReady()) {
        // A unit that resolves wins; anything else falls back to the target,
        // which is what an ordinary click has always done. An unresolvable
        // unit is not treated as "cast on nobody" - a raid frame naming a
        // member who has gone out of range should still cast at the target
        // rather than silently at nothing.
        uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
        if (const char* unit = lua_tostring(L, 2); unit && *unit) {
            std::string uid(unit);
            toLowerInPlace(uid);
            if (const uint64_t named = resolveUnitGuid(gh, uid)) target = named;
        }
        // A spell that belongs to an item - a Hearthstone is spell 8690 - has
        // to go as CMSG_USE_ITEM rather than a cast, or the server drops it.
        // This client's own action bar did that and was the only place that
        // did; the bar is FrameXML's now and this is the path its buttons take.
        if (const uint32_t itemForSpell = gh->getItemIdForSpell(action.id)) {
            gh->useItemById(itemForSpell, target);
        } else {
            gh->castSpell(action.id, target);
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        gh->useItemById(action.id);
    } else if (action.type == game::ActionBarSlot::MACRO) {
        // Run as RunMacro runs one, through the client's macro runner: one
        // line at a time through the slash commands, /stopmacro honoured.
        static std::set<uint32_t> said;
        const game::Macro* macro = gh->accountData().macro(action.id);
        auto* svc = getLuaServices(L);
        if (!macro) {
            if (said.insert(action.id).second) {
                LOG_WARNING("UseAction: slot ", slot + 1, " holds macro ", action.id,
                            ", which the account data does not have; nothing runs");
            }
        } else if (!svc || !svc->runMacroText) {
            if (said.insert(action.id).second) {
                LOG_WARNING("UseAction: macro \"", macro->name,
                            "\" not run: this client has no macro runner");
            }
        } else if (!macro->body.empty()) {
            svc->runMacroText(macro->body);
        }
    }
    return 0;
}

// --- Cursor / Drag-Drop System ---
// Tracks what the player is "holding" on the cursor (spell, item, action).


static int lua_ClearCursor(lua_State* L) {
    // Everything a pickup set, including the icon drawn on the pointer.
    //
    // This cleared the type, the id, the slot and the bag and left the icon,
    // which is a separate piece of state that only clearCursorItem touches. So
    // cancelling a pickup - Escape, a right-click, a click on nothing, all of
    // which reach this - put the item back and left its picture stuck to the
    // cursor for the rest of the session.
    clearCursorItem(L);
    return 0;
}

static int lua_GetCursorInfo(lua_State* L) {
    switch (cursor(L).type) {
        case CursorType::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, 0);          // bookSlotIndex
            lua_pushstring(L, "spell");    // bookType
            lua_pushnumber(L, cursor(L).id); // spellId
            return 4;
        case CursorType::ITEM:
        // An item out of the guild bank is an item. Where it came from is this
        // file's business - PickupContainerItem reads the cursor type to know a
        // drop is a withdrawal - and saying so here would only give the
        // interface a kind it has no branch for.
        case CursorType::GUILDBANK:
            lua_pushstring(L, "item");
            lua_pushnumber(L, cursor(L).id);
            return 2;
        case CursorType::MACRO:
            lua_pushstring(L, "macro");
            lua_pushnumber(L, macroIndexOf(getGameHandler(L), cursor(L).id));
            return 2;
        case CursorType::MONEY:
            // containerframe.lua branches on this exact string to decide that
            // a click in a bag is money being put back.
            lua_pushstring(L, "money");
            lua_pushnumber(L, static_cast<lua_Number>(cursor(L).money));
            return 2;
        case CursorType::MERCHANT:
            // containerframe.lua branches on this exact string to decide
            // whether a click in a bag is a purchase.
            lua_pushstring(L, "merchant");
            lua_pushnumber(L, cursor(L).id);   // the vendor's list index
            return 2;
        default:
            return 0;
    }
}

static int lua_CursorHasItem(lua_State* L) {
    // A guild bank item counts. The bank frame's own OnClick tests this before
    // deciding a click is a drop, so answering no for one would have left the
    // item stuck on the cursor with nowhere that would take it.
    lua_pushboolean(L, (cursor(L).type == CursorType::ITEM ||
                        cursor(L).type == CursorType::GUILDBANK) ? 1 : 0);
    return 1;
}

static int lua_CursorHasSpell(lua_State* L) {
    lua_pushboolean(L, cursor(L).type == CursorType::SPELL ? 1 : 0);
    return 1;
}

// PickupAction(slot) - picks up an action from the action bar
/// PickupAction(slot) - take what is in a slot onto the cursor, and put down
/// what the cursor was already holding.
///
/// A swap, which is what the real client does and what the interface expects:
/// ActionButton_OnClick calls this for a click on a button whether or not
/// anything is held.
///
/// The slot used to be left exactly as it was. Picking an action up put a copy
/// on the cursor and the button went on showing it, so dragging something off
/// the bar looked like it had done nothing at all - and because the slot never
/// changed, no ACTIONBAR_SLOT_CHANGED was fired and nothing redrew. The empty
/// half only understood a spell, so an item or a macro dropped on a free slot
/// vanished.
static int lua_PickupAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    const auto& bar = gh->getActionBar();
    if (slot < 1 || slot > static_cast<int>(bar.size())) return 0;

    // Read before anything is written: setActionBarSlot rewrites the entry
    // this would otherwise still be pointing at.
    const auto existing = bar[slot - 1];
    const bool hadAction = !existing.isEmpty();
    const CursorType held = cursor(L).type;
    const uint32_t heldId = cursor(L).id;

    const auto typeForCursor = [](CursorType c) {
        return c == CursorType::SPELL ? game::ActionBarSlot::SPELL
             : c == CursorType::ITEM  ? game::ActionBarSlot::ITEM
                                      : game::ActionBarSlot::MACRO;
    };
    const bool holding = heldId != 0 &&
                         (held == CursorType::SPELL || held == CursorType::ITEM ||
                          held == CursorType::MACRO);

    if (holding) {
        gh->setActionBarSlot(slot - 1, typeForCursor(held), heldId);
    } else if (hadAction) {
        // Nothing held, so the slot empties. This is the fire that redraws the
        // button: setActionBarSlot raises ACTIONBAR_SLOT_CHANGED for it.
        gh->setActionBarSlot(slot - 1, game::ActionBarSlot::EMPTY, 0);
    }

    if (hadAction) {
        // What was there goes to the cursor, whether this was a swap or a
        // plain pick-up. existing.type is never EMPTY here (hadAction), so
        // this covers every case - CursorType::ACTION was a dead end lua_
        // PlaceAction had no branch for, and picking a macro up off the bar
        // fell to it by default: the macro left its slot, rode the cursor as
        // an unplaceable "action", and vanished on drop instead of landing
        // anywhere.
        setCursorType(L, (existing.type == game::ActionBarSlot::SPELL) ? CursorType::SPELL :
                       (existing.type == game::ActionBarSlot::ITEM)  ? CursorType::ITEM :
                       CursorType::MACRO);
        cursor(L).id = existing.id;
        cursor(L).slot = slot;
        // Which button, not which slot of which bag - a fourth numbering, and
        // the reason the source has to say so. An item action carries a real
        // itemId, so without this the cursor was indistinguishable from one
        // lifted off the paperdoll and the button number was spent as an
        // equipment slot. See kCursorNoSource.
        cursor(L).bag = game::slots::kCursorNoSource;
        // The shared cursor as well, which is a separate record of the same
        // thing and would otherwise still be naming wherever the *previous*
        // pickup came from: clicking a button while carrying a bag item swaps
        // the two, and the bag slot it left behind stayed on this cursor as
        // the source PutItemInBag would have moved from.
        cursor(L).item = {};
        // And on the pointer, or the drag reads as not having started.
        if (existing.type == game::ActionBarSlot::SPELL) {
            wowee::ui::frameXmlSetCursorItem(gh->getSpellIconPath(existing.id));
        } else if (existing.type == game::ActionBarSlot::ITEM) {
            const auto* info = gh->getItemInfo(existing.id);
            wowee::ui::frameXmlSetCursorItem(
                info ? gh->getItemIconPath(info->displayInfoId) : std::string());
        } else {
            const auto* macro = gh->accountData().macro(existing.id);
            wowee::ui::frameXmlSetCursorItem(macro ? macroTexture(*macro) : std::string(kQuestionMarkIcon));
        }
    } else {
        clearCursorItem(L);
    }
    return 0;
}

// PlaceAction(slot) - places cursor content into an action bar slot
//
// A swap, as the real one is: whatever the slot held goes onto the cursor, so
// a bar can be rearranged by dropping one action on another and then placing
// the one that came off. This wrote the new action over the old and cleared
// the cursor, so the action underneath was simply gone - and it is PlaceAction
// an action button's OnReceiveDrag calls, so every drop onto an occupied
// button lost what was there.
//
// PickupAction while holding something is exactly that swap, so this is it.
// With nothing placeable held it does nothing, and leaves the cursor alone.
static int lua_PlaceAction(lua_State* L) {
    const bool holding = cursor(L).id != 0 &&
                         (cursor(L).type == CursorType::SPELL ||
                          cursor(L).type == CursorType::ITEM ||
                          cursor(L).type == CursorType::MACRO);
    if (!holding) return 0;
    return lua_PickupAction(L);
}

// PickupSpell(bookSlot, bookType) - picks up a spell from the spellbook
static int lua_PickupSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    // Through the shared resolver, so the book is read as well as the slot:
    // dragging a pet spell out of the book picked up whichever of the player's
    // spells sat at that index.
    const uint32_t picked = spellIdForCall(L, gh);
    if (picked == 0) return 0;
    setCursorType(L, CursorType::SPELL);
    cursor(L).id = picked;
    // On the pointer as well as in the state, the way every other pickup here
    // does it. Dragging a spell out of the book carried nothing visible, so
    // the drag read as not having started.
    wowee::ui::frameXmlSetCursorItem(gh->getSpellIconPath(cursor(L).id));
    return 0;
}

// PlaceGlyphInSocket(socket) - put the glyph on the cursor into that socket.
//
// Registered here rather than with the other glyph calls because the glyph is
// on the cursor and the cursor lives in this file. There is no socketing opcode
// in 3.3.5: the glyph item is used with the socket written into the glyphIndex
// field CMSG_USE_ITEM already carries, and the server applies the item's spell
// to it. The comment this replaces said socketing "needs a packet this client
// does not send" - the packet was there, one field short of saying where.
static int lua_PlaceGlyphInSocket(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int socket = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || socket < 1 || socket > game::GameHandler::MAX_GLYPH_SLOTS) return 0;
    uint8_t bag = 0, slot = 0;
    if (!cursorWireSlot(L, bag, slot)) return 0;   // nothing held: nothing to place
    gh->placeGlyphFromBag(bag, slot, static_cast<uint32_t>(socket - 1));
    setCursorType(L, CursorType::NONE);
    return 0;
}

// PickupGuildBankItem(tab, slot) - both halves of a guild bank drag.
//
// The frame calls this from OnDragStart and from OnReceiveDrag, so one function
// picks up and puts down, the way PickupContainerItem does for bags. It was a
// no-op, so nothing could be moved into or out of the guild bank at all -
// while the packet builder for the move sat verified against the server, and
// AutoStoreGuildBankItem beside it has been using half of it for right-clicks.
//
// The tab and slot are FrameXML's, counted from one; the wire counts from zero,
// which is the shift AutoStoreGuildBankItem already applies.
static int lua_PickupGuildBankItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int tab = static_cast<int>(luaL_optnumber(L, 1, 0));
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || tab < 1 || slot < 1) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying a bag item, so this is the deposit.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(L, srcBag, srcSlot)) {
        gh->guildBankDepositItem(static_cast<uint8_t>(tab - 1),
                                 static_cast<uint8_t>(slot - 1), srcBag, srcSlot);
        setCursorType(L, CursorType::NONE);
        return 0;
    }
    // Carrying a bank item already: that is a move within the bank, and the
    // request builder says plainly that it does not write that path. Dropping
    // the cursor is better than sending something the server will read as a
    // withdrawal to nowhere.
    if (cursor(L).type == CursorType::GUILDBANK) {
        setCursorType(L, CursorType::NONE);
        return 0;
    }

    // Otherwise pick the slot up, if there is anything in it.
    const auto& bank = gh->getGuildBankData();
    for (const auto& it : bank.tabItems) {
        if (it.slotId != slot - 1 || it.itemEntry == 0) continue;
        setCursorType(L, CursorType::GUILDBANK);
        cursor(L).id = it.itemEntry;
        cursor(L).bag = tab;
        cursor(L).slot = slot;
        cursor(L).split = 0;   // the whole stack, unless SplitGuildBankItem says otherwise
        const auto* info = gh->getItemInfo(it.itemEntry);
        wowee::ui::frameXmlSetCursorItem(
            info ? gh->getItemIconPath(info->displayInfoId) : std::string());
        return 0;
    }
    return 0;
}

// SplitGuildBankItem(tab, slot, amount) - take part of a stack.
//
// The stack split dialog calls this instead of PickupGuildBankItem when the
// player asks for fewer than all of them. It is the same pickup with a number
// attached: the wire carries the split on the *withdrawal* rather than on the
// pickup, so the amount rides on the cursor until the drop says where it goes.
static int lua_SplitGuildBankItem(lua_State* L) {
    const int amount = static_cast<int>(luaL_optnumber(L, 3, 0));
    lua_settop(L, 2);
    lua_PickupGuildBankItem(L);
    if (cursor(L).type == CursorType::GUILDBANK && amount > 0)
        cursor(L).split = static_cast<uint32_t>(amount);
    return 0;
}

// PickupCompanion(mode, index) - drag a mount or a critter to the bar.
//
// Registered here rather than beside the other companion calls because the
// cursor lives in this file, and because that is what this is: a pickup.
//
// A companion is summoned by a spell, and this client's action bar holds
// spells, so the cursor carries the summon rather than a companion of its own
// kind. What lands on the bar is the same icon casting the same thing; the
// difference is invisible to everyone but the tooltip, which names the spell.
// Modelling a fourth action type to say "companion" would change nothing a
// player can see.
static int lua_PickupCompanion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* mode = luaL_optstring(L, 1, "");
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (index < 1) return 0;
    const auto& list = gh->getCompanions(std::string(mode) == "MOUNT");
    if (index > static_cast<int>(list.size())) return 0;
    const uint32_t spellId = list[static_cast<size_t>(index) - 1].spellId;
    if (spellId == 0) return 0;
    setCursorType(L, CursorType::SPELL);
    cursor(L).id = spellId;
    wowee::ui::frameXmlSetCursorItem(gh->getSpellIconPath(spellId));
    return 0;
}

// PickupSpellBookItem(bookSlot, bookType) - alias for PickupSpell
static int lua_PickupSpellBookItem(lua_State* L) {
    return lua_PickupSpell(L);
}

// PickupContainerItem(bag, slot) - picks up an item from a bag

/// Where the item on the cursor lives, in the numbering the server uses.
///
/// The server addresses one flat space: equipment is slots 0 to 22, the
/// backpack follows at 23, and a bag's contents are addressed by that bag's own
/// equipment slot together with an index inside it. FrameXML counts bags 0 to 4
/// with 1-based slots instead, so the two have to be translated - the same
/// translation this client's own bag window does before sending a swap.
static bool cursorWireSlot(lua_State* L, uint8_t& bag, uint8_t& slot) {
    if (cursor(L).type != CursorType::ITEM) return false;
    // Nowhere to move it from. This used to fall into the equipped branch
    // below, because everything negative was the paperdoll - see
    // kCursorNoSource for what that cost.
    if (!game::slots::cursorSourceIsInventory(cursor(L).bag)) return false;

    // Minus one is two different places. The paperdoll picks an item up with
    // it, and the interface numbers the bank's own slots BANK_CONTAINER, which
    // is also minus one - so an item lifted out of the bank read as a worn one,
    // and the move that followed named an equipment slot: bank slot five went
    // out as equipment slot four. The flag the cursor already carries is the
    // only thing that tells the two apart, and it was not being read.
    if (cursor(L).item.equipped) {
        bag = 0xFF;
        slot = static_cast<uint8_t>(cursor(L).slot - 1);
        return true;
    }
    // Everything else through the shared pair, which knows the backpack, the
    // keyring, the bank, its bags and the worn bags. Written out here it knew
    // three of them.
    bag = containerWireBag(cursor(L).bag);
    slot = containerWireSlot(cursor(L).bag, cursor(L).slot);
    return true;
}

/// Put the cursor down.
static void clearCursorItem(lua_State* L) {
    cursor(L).money = 0;
    cursor(L).split = 0;
    setCursorType(L, CursorType::NONE);
    cursor(L).id = 0;
    cursor(L).slot = 0;
    cursor(L).bag = -1;
    wowee::ui::frameXmlSetCursorItem(std::string());
    cursor(L).item = {};
}

static bool droppedItemFromNowhere(lua_State* L) {
    if (cursor(L).type != CursorType::ITEM) return false;
    if (game::slots::cursorSourceIsInventory(cursor(L).bag)) return false;
    clearCursorItem(L);
    return true;
}

void pickupMerchantItem(lua_State* L, int index) {
    // Index zero is not a pickup at all: it means "put what the cursor is
    // holding into the merchant's sell slot", which is how 3.3.5 sells from a
    // bag. ContainerFrameItemButton_OnClick picks the item up and calls this
    // with no argument, and luaL_optnumber turns that into the zero below.
    //
    // This returned here, so a right-click at a vendor lifted the item onto
    // the cursor, asked to sell it, was answered with nothing, and left it
    // stuck to the pointer. ShowContainerSellCursor beside this is the other
    // half of the same path and is a no-op for a reason - it only changes the
    // cursor art - which is what made this one look deliberate too.
    if (index < 1) {
        soldHeldItem(L);
        return;
    }
    setCursorType(L, CursorType::MERCHANT);
    cursor(L).id = static_cast<uint32_t>(index);
    cursor(L).slot = 0;
    cursor(L).bag = -1;
    // Show it on the pointer, as a bag pickup does. Without this the cursor
    // holds something invisible and the click reads as having done nothing.
    if (auto* gh = getGameHandler(L)) {
        const auto& items = gh->getVendorItems().items;
        if (index <= static_cast<int>(items.size())) {
            ui::frameXmlSetCursorItem(
                gh->getItemIconPath(items[index - 1].displayInfoId));
        }
    }
}

/// Sell whatever the cursor is holding, and put it down either way.
///
/// Bags and the backpack only. An item on the paperdoll can be dragged to a
/// merchant in the real client, but the two sell verbs here are the ones that
/// keep the buyback list, and neither knows an equipment slot - inventing a
/// third path for a case a right-click in a bag cannot reach would be guessing
/// at the bookkeeping rather than restoring behaviour.
static void soldHeldItem(lua_State* L) {
    // A merchant item on the cursor was never taken out of anything, so there
    // is nothing to sell and nothing to put back.
    if (cursor(L).type == CursorType::MERCHANT) { clearCursorItem(L); return; }
    if (cursor(L).type != CursorType::ITEM) return;
    auto* gh = getGameHandler(L);
    if (!gh) return;
    if (!game::slots::cursorSourceIsInventory(cursor(L).bag)) return;

    if (cursor(L).bag == 0) {
        gh->sellItemBySlot(cursor(L).slot - 1);
    } else if (cursor(L).bag > 0) {
        gh->sellItemInBag(cursor(L).bag - 1, cursor(L).slot - 1);
    } else {
        return;   // worn, and held rather than dropped on the floor
    }
    clearCursorItem(L);
}

bool boughtHeldMerchantItem(lua_State* L) {
    if (cursor(L).type != CursorType::MERCHANT) return false;
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(cursor(L).id);
    if (gh && index >= 1) {
        const auto& items = gh->getVendorItems().items;
        if (index <= static_cast<int>(items.size())) {
            const auto& vi = items[index - 1];
            gh->buyItem(gh->getVendorGuid(), vi.itemId, vi.slot, 1);
        }
    }
    clearCursorItem(L);
    return true;
}


/// Put the item in a bag slot on the cursor, with everything that goes with it:
/// the icon, the greyed slot, and the record of where it came from.
///
/// Split out of lua_PickupContainerItem so PickupItem can reach it. Writing the
/// cursor twice is how one of them would come to set the icon and the other not.
static void pickupFromContainerSlot(lua_State* L, game::GameHandler* gh,
                                    int bag, int slot) {
    const auto& inv = gh->getInventory();
    // Every container this numbering reaches, not the backpack and the four
    // worn bags alone: the bank's own slots are container -1 and its bags are
    // 5 through 11, and bankframe.lua picks an item up with
    // PickupContainerItem(BANK_CONTAINER, self:GetID()). Missing them, the bank
    // answered "nothing there to pick up" for every square that had something
    // in it.
    const game::ItemSlot* itemSlot = containerItemSlot(inv, bag, slot);
    if (itemSlot && !itemSlot->empty()) {
        setCursorType(L, CursorType::ITEM);
        cursor(L).id = itemSlot->item.itemId;
        cursor(L).bag = bag;
        cursor(L).slot = slot;
        uint32_t displayId = itemSlot->item.displayInfoId;
        if (displayId == 0) {
            if (const auto* info = gh->getItemInfo(itemSlot->item.itemId)) {
                displayId = info->displayInfoId;
            }
        }
        wowee::ui::frameXmlSetCursorItem(
            displayId ? gh->getItemIconPath(displayId) : std::string());
        cursor(L).item = {bag, slot, false};
        // The slot draws greyed while its item is on the cursor, which is how a
        // bag shows that something has been picked up out of it.
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(bag), std::to_string(slot)});
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " item ", itemSlot->item.itemId);
    } else {
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " - nothing there to pick up");
    }
}

void pickupSplitFromContainer(lua_State* L, game::GameHandler* gh,
                              int bag, int slot, int count) {
    pickupFromContainerSlot(L, gh, bag, slot);
    // Only if the pickup took: an empty slot leaves the cursor alone, and an
    // amount left on it would ride along with whatever is picked up next.
    if (cursor(L).type == CursorType::ITEM && count > 0) {
        cursor(L).split = static_cast<uint32_t>(count);
    }
}


/// PickupItem(id | "name" | link) - put an item on the cursor by naming it
/// rather than by pointing at a slot.
///
/// A macro's own way to pick something up, and the 'item' branch of
/// securehandlers.lua's cursor dispatcher - every other branch there was bound
/// and this one raised.
///
/// The first matching slot wins. WoW picks the first too, and a stack split
/// across two bags is one item as far as this is concerned.
static int lua_ReturnNothing2(lua_State* L) { (void)L; return 0; }

static int lua_PickupItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    uint32_t wantId = 0;
    std::string wantName;
    if (lua_isnumber(L, 1)) {
        wantId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (const char* text = lua_tostring(L, 1)) {
        // A link carries the id in it - "|Hitem:6948:0:..." - and is what a
        // shift-click puts in a macro, so it is the common case rather than
        // the exotic one.
        const std::string str(text);
        const size_t at = str.find("Hitem:");
        if (at != std::string::npos) {
            wantId = static_cast<uint32_t>(std::strtoul(str.c_str() + at + 6, nullptr, 10));
        } else {
            wantName = str;
        }
    }
    if (wantId == 0 && wantName.empty()) return 0;

    auto matches = [&](const game::ItemSlot& s) {
        if (s.empty()) return false;
        if (wantId != 0) return s.item.itemId == wantId;
        return s.item.name == wantName;
    };

    const auto& inv = gh->getInventory();
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        if (matches(inv.getBackpackSlot(i))) {
            pickupFromContainerSlot(L, gh, 0, i + 1);
            return 0;
        }
    }
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
        for (int sl = 0; sl < inv.getBagSize(b); ++sl) {
            if (matches(inv.getBagSlot(b, sl))) {
                pickupFromContainerSlot(L, gh, b + 1, sl + 1);
                return 0;
            }
        }
    }
    return 0;
}

static int lua_PickupContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int bag = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));

    // Same as the paperdoll: a click while something is waiting for an item to
    // be applied to is that choice, not a pickup.
    if (completedItemTarget(L, containerSlotGuid(gh, bag, slot))) return 0;

    // With the repair cursor up, a click on an item is the repair. The real
    // client consumes the click in C for the same reason, which is why
    // containerframe.lua has no branch of its own for it.
    if (repairedHeldItem(L, gh, containerSlotGuid(gh, bag, slot))) return 0;

    // A bag click while the cursor holds a vendor's item is the purchase.
    // containerframe.lua routes it here after checking GetCursorInfo.
    if (boughtHeldMerchantItem(L)) return 0;

    // Carrying something out of the guild bank, so this is the withdrawal.
    // The destination is this bag slot rather than the server's choice, which
    // is what dropping on a particular square means.
    if (cursor(L).type == CursorType::GUILDBANK) {
        const uint8_t dstBag = containerWireBag(bag);
        const uint8_t dstSlot = containerWireSlot(bag, slot);
        gh->guildBankWithdrawItem(static_cast<uint8_t>(cursor(L).bag - 1),
                                  static_cast<uint8_t>(cursor(L).slot - 1),
                                  dstBag, dstSlot, cursor(L).split);
        setCursorType(L, CursorType::NONE);
        return 0;
    }

    // Carrying something the bags cannot take: the drag ends here and nothing
    // moves. Before cursorWireSlot, which would otherwise answer no and let
    // this fall through to the pickup below - putting the bag's own item on a
    // cursor that is already holding one.
    if (droppedItemFromNowhere(L)) return 0;

    // Already carrying something, so this is the drop rather than the pickup.
    // One function does both halves of a drag in WoW, and without this half a
    // dragged item was picked up and never put down anywhere.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(L, srcBag, srcSlot)) {
        // Through the shared pair rather than written out here. This knew the
        // backpack and treated everything else as a worn bag, so a drop into
        // the bank - container -1 - asked wornBagContainer for container 17 and
        // sent the move to a container that is not one. Nothing in the bank
        // window could be dragged anywhere, including a bag sitting in it.
        const uint8_t dstBag = containerWireBag(bag);
        const uint8_t dstSlot = containerWireSlot(bag, slot);
        // At warning level because it is the outcome of a drag and happens
        // once per drop, and because the log carries nothing below warning -
        // which is why "did the move go out" could not be answered at all.
        LOG_WARNING("FrameXML drop: bag ", cursor(L).bag, " slot ", cursor(L).slot,
                    " (wire ", (int)srcBag, "/", (int)srcSlot, ") -> bag ", bag,
                    " slot ", slot, " (wire ", (int)dstBag, "/", (int)dstSlot, ")");
        // Back where it came from: put it down rather than asking the server to
        // swap a slot with itself.
        //
        // The equipped flag counts here too, for the same reason it counts
        // above: a worn item on the cursor carries bag minus one, and so does
        // the bank, so dropping the helm on bank slot one read as putting it
        // back where it came from and quietly did nothing.
        if (cursor(L).bag == bag && cursor(L).slot == slot && !cursor(L).item.equipped) {
            const int sameBag = bag, sameSlot = slot;
            clearCursorItem(L);
            gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                               {std::to_string(sameBag), std::to_string(sameSlot)});
            return 0;
        }

        const int wasBag = cursor(L).bag, wasSlot = cursor(L).slot;
        // Part of a stack moves as a split rather than a swap: a swap would
        // send the whole thing, which is not what was picked up.
        if (cursor(L).split > 0) {
            gh->splitItemTo(srcBag, srcSlot, dstBag, dstSlot,
                            static_cast<uint8_t>(cursor(L).split));
        } else {
            gh->swapContainerItems(srcBag, srcSlot, dstBag, dstSlot);
        }
        clearCursorItem(L);
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(wasBag), std::to_string(wasSlot)});
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(bag), std::to_string(slot)});
        return 0;
    }

    pickupFromContainerSlot(L, gh, bag, slot);
    return 0;
}

// PickupInventoryItem(slot) - picks up an equipped item
// ClickSendMailItemButton(index, isRightClick) - attach what is held to the
// letter, or take an attachment back.
//
// Lives here rather than with the other mail functions because the cursor
// does, and this is entirely about the cursor: carrying an item means attach
// it, carrying nothing means the slot clicked gives its item back to the bags.
// The attachment index is optional - the drop handler calls this with no
// arguments at all, meaning "attach to wherever there is room", which is what
// attachItemFrom* already does.
static int lua_ClickSendMailItemButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    if (cursor(L).type == CursorType::ITEM) {
        // The cursor counts the backpack as bag zero and the worn bags from
        // one, with slots from one; the attach functions count both from zero.
        const bool attached = (cursor(L).bag == 0)
            ? gh->attachItemFromBackpack(cursor(L).slot - 1)
            : (cursor(L).bag > 0
                   ? gh->attachItemFromBag(cursor(L).bag - 1, cursor(L).slot - 1)
                   : false);
        if (attached) clearCursorItem(L);
        return 0;
    }

    // Nothing held: the click takes an attachment off the letter. Without an
    // index there is nothing to take off, since the caller meant a drop.
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (index >= 1) gh->detachMailAttachment(index - 1);
    return 0;
}

// PickupBagFromSlot(inventorySlot) - pick up or put down a bank bag.
//
// The bank's bag buttons hand over an inventory slot rather than an index:
// BankButtonIDToInvSlotID puts the seven bank bags at sixty-eight upward, and
// the wire counts from zero, so the slot sent is one less. That is the same
// arithmetic PickupInventoryItem does for worn equipment, and the cursor
// carries it the same way - a negative bag means "not in a container", which
// cursorWireSlot turns back into container 0xFF and the slot less one.
//
// Deferred for a long time on the belief that this client had no cursor to
// drop things from. It has had one all along, with both halves of a drag in
// one function, and the wire numbers here are the ones its own bank window
// already sends.
/// The nth worn bag, as Lua numbers inventory slots - the four beside the
/// backpack. BagSlotButton_OnDrag passes exactly these.
static constexpr int kFirstWornBagInventorySlot =
    game::slots::toInventorySlot(game::slots::wornBagContainer(0));

static int lua_PickupBagFromSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));

    const int bankIndex = slot - game::slots::firstBankBagInventorySlot();
    const int wornIndex = slot - kFirstWornBagInventorySlot;
    const bool isBank = bankIndex >= 0 && bankIndex < game::slots::bankBagCount();
    const bool isWorn = wornIndex >= 0 && wornIndex < game::Inventory::NUM_BAG_SLOTS;
    // Only the bank half was handled, so dragging one of the four worn bags did
    // nothing at all - it returned here before doing anything. Latent until the
    // bag bar was handed over, because this client's own bar never called it.
    if (!isBank && !isWorn) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying something: this is the drop, into the bag slot.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(L, srcBag, srcSlot)) {
        const int srcWorn = cursor(L).slot - kFirstWornBagInventorySlot;
        const bool fromWorn = cursor(L).bag == -1 &&
                              srcWorn >= 0 && srcWorn < game::Inventory::NUM_BAG_SLOTS;
        if (fromWorn && isWorn) {
            // Both are worn bags, which has its own verb: it swaps the bags'
            // contents locally as well as sending the move, so the bags on
            // screen do not have to wait for the server to agree.
            gh->swapBagSlots(srcWorn, wornIndex);
        } else {
            gh->swapContainerItems(srcBag, srcSlot, 0xFF,
                                   static_cast<uint8_t>(game::slots::toWireSlot(slot)));
        }
        clearCursorItem(L);
        return 0;
    }

    const auto& held = isBank
        ? gh->getInventory().getBankBagItem(bankIndex)
        : gh->getInventory().getEquipSlot(static_cast<game::EquipSlot>(
              static_cast<int>(game::EquipSlot::BAG1) + wornIndex));
    if (held.empty()) {
        // Dragging a bag that is plainly in the slot has been reported doing
        // nothing, and every step of the arithmetic checks out on paper: the
        // button's id, the worn index, the equipment slot. So say what the
        // slot actually held when the drag arrived, rather than returning in
        // silence as this has always done for an empty slot.
        LOG_WARNING("PickupBagFromSlot: slot ", slot, " (worn index ", wornIndex,
                    ", bank index ", bankIndex, ") reads as empty - nothing to pick up");
        return 0;
    }
    setCursorType(L, CursorType::ITEM);
    cursor(L).id = held.item.itemId;
    cursor(L).slot = slot;
    cursor(L).bag = -1;
    // The shared cursor as well as this file's own. Everything that receives a
    // drop reads cursor(L).item, and a worn bag is an equipped item like any
    // other - PickupInventoryItem below sets exactly this. Without it the bag
    // lifted onto the pointer and then every drop target reported nothing
    // held: PutItemInBag returned false, so BagSlotButton_OnClick fell through
    // to ToggleBag and the bag opened instead of moving.
    cursor(L).item = {-1, slot, true};
    // Same omission as the spell above: a bag dragged off its slot was held
    // invisibly. The display id falls back to the item's own, which is where
    // the equipped-item pickup below reads it from when the slot has none.
    uint32_t displayId = held.item.displayInfoId;
    if (displayId == 0) {
        if (const auto* info = gh->getItemInfo(held.item.itemId)) {
            displayId = info->displayInfoId;
        }
    }
    wowee::ui::frameXmlSetCursorItem(
        displayId ? gh->getItemIconPath(displayId) : std::string());
    // And say the slot is locked, as the equipped pickup below does.
    // IsInventoryItemLocked already answers true for it - cursorEquipSlot
    // reads the same cursor(L).slot this just set - but nothing asks again
    // without this, so the square greyed only on the next unrelated redraw.
    // One argument, because the paperdoll's handler tests `not arg2`.
    gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
    return 0;
}

static int lua_PickupInventoryItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (slot < 1 || slot > 23) return 0;

    // Before the pickup, because the paperdoll has no SpellCanTargetItem branch
    // of its own - a left-click on a worn weapon always arrives here, and while
    // a stone or an oil is waiting for a target that click is the target.
    if (slot <= 19 && completedItemTarget(L, gh->getEquipSlotGuid(slot - 1))) return 0;

    // Worn gear is what usually needs repairing, and the paperdoll has no
    // branch of its own for it either.
    if (slot <= 19 && repairedHeldItem(L, gh, gh->getEquipSlotGuid(slot - 1))) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying something: equip it here, which is the other half of the drag.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(L, srcBag, srcSlot)) {
        gh->swapContainerItems(srcBag, srcSlot, 0xFF,
                               static_cast<uint8_t>(slot - 1));
        clearCursorItem(L);
        return 0;
    }

    const auto& inv = gh->getInventory();
    const auto& eq = inv.getEquipSlot(static_cast<game::EquipSlot>(slot - 1));
    if (!eq.empty()) {
        setCursorType(L, CursorType::ITEM);
        cursor(L).id = eq.item.itemId;
        cursor(L).slot = slot;
        cursor(L).bag = -1;
        // The same three things a container pickup does, which this had none
        // of: remember where it came from, put its icon on the pointer, and say
        // the slot is locked. Without them dragging off the character sheet
        // carried nothing visible and left the slot looking untouched.
        cursor(L).item = {-1, slot, true};
        uint32_t displayId = eq.item.displayInfoId;
        if (displayId == 0) {
            if (const auto* info = gh->getItemInfo(eq.item.itemId)) {
                displayId = info->displayInfoId;
            }
        }
        wowee::ui::frameXmlSetCursorItem(
            displayId ? gh->getItemIconPath(displayId) : std::string());
        // One argument, not two: the paperdoll's handler is
        // `if ( not arg2 and arg1 == self:GetID() )`, so an equipment lock is
        // the slot alone. A second argument makes that test fail and the square
        // never greys.
        gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
        LOG_WARNING("FrameXML pickup: equipment slot ", slot,
                    " item ", eq.item.itemId);
    } else {
        LOG_WARNING("FrameXML pickup: equipment slot ", slot,
                    " - nothing equipped there");
    }
    return 0;
}

// DeleteCursorItem() - destroys the item on the cursor.
//
// It only put the cursor down. Nothing was ever destroyed: the item vanished
// from the cursor, the server was never told, and the next bag update put it
// straight back. StaticPopup "DELETE_ITEM" calls this on Accept, so the whole
// drag-an-item-out-to-destroy-it gesture ended in the item reappearing.
//
// cursorWireSlot already converts the held position into the bag and slot the
// server expects, which is the same translation the drop half of a drag uses.
static int lua_DeleteCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint8_t bag = 0, slot = 0;
    if (gh && cursorWireSlot(L, bag, slot)) {
        // The whole stack: WoW asks about a partial stack before it gets here,
        // and the popup that calls this is the confirmation for all of it.
        gh->destroyItem(bag, slot, 0);
    }
    clearCursorItem(L);
    return 0;
}

// AutoEquipCursorItem() - equip item from cursor
static int lua_AutoEquipCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && cursor(L).type == CursorType::ITEM && cursor(L).id != 0) {
        gh->useItemById(cursor(L).id);
    }
    clearCursorItem(L);
    return 0;
}

// --- Frame System ---
// Minimal WoW-compatible frame objects with RegisterEvent/SetScript/GetScript.
// Frames are Lua tables with a metatable that provides methods.

// Frame method: frame:RegisterEvent("EVENT")

// The modifier state comes from the keyboard rather than from ImGui, which
// learns it from events this client does not always route there once FrameXML
// owns the mouse. lua_system_api registers the same three names against
// SDL_GetModState; these agree with it now instead of racing it, and the
// duplicate Is*KeyDown bindings that used to live here are gone with them.
static bool shiftHeld() { return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0; }
static bool ctrlHeld()  { return (SDL_GetModState() & SDL_KMOD_CTRL)  != 0; }
static bool altHeld()   { return (SDL_GetModState() & SDL_KMOD_ALT)   != 0; }


/// What bindings.xml declares for a modified-click action, or empty.
///
/// One list, read once. The emitter keeps every <ModifiedClick action= default=>
/// in __WoweeModifiedClick, which is the same file the key bindings panel is
/// built from - so this cannot drift from what the interface believes.
///
/// It had drifted. A table written out by hand here answered ALT for FOCUSCAST,
/// which bindings.xml declares as NONE, so alt-clicking a spell tried to cast
/// it at the focus when nothing had asked for that; and both flyout actions are
/// declared ALT and fell through to a shift default.
static std::string modifiedClickBinding(lua_State* L, const std::string& action) {
    lua_getglobal(L, "__WoweeModifiedClick");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    lua_getfield(L, -1, action.c_str());
    const char* value = lua_tostring(L, -1);
    std::string out = value ? value : "";
    lua_pop(L, 2);
    return out;
}

/// Whether the modifiers a binding names are all held.
///
/// A binding is a dash-separated list ending in an optional button -
/// "SHIFT-BUTTON1", "CTRL", "NONE". The button half is the caller's business:
/// it already knows which button was pressed, and every caller here is inside
/// a handler for one.
static bool modifiersHeldFor(const std::string& binding) {
    if (binding.empty() || binding == "NONE") return false;
    bool wantShift = false, wantCtrl = false, wantAlt = false;
    std::string wantButton;
    size_t at = 0;
    while (at <= binding.size()) {
        const size_t dash = binding.find('-', at);
        const std::string part = binding.substr(
            at, dash == std::string::npos ? std::string::npos : dash - at);
        if (part == "SHIFT") wantShift = true;
        else if (part == "CTRL") wantCtrl = true;
        else if (part == "ALT") wantAlt = true;
        // A binding names a button as often as it names a modifier, and the
        // two named actions on the same modifier are told apart by nothing
        // else: CHATLINK is SHIFT-BUTTON1 and SOCKETITEM is SHIFT-BUTTON2.
        // Reading only the modifier made both true on any shift-click, so
        // shift-clicking an item in a bag put the link in chat and opened the
        // socketing window behind it - or, when the link could not be
        // inserted, only the socketing window.
        else if (part == "BUTTON1") wantButton = "LeftButton";
        else if (part == "BUTTON2") wantButton = "RightButton";
        if (dash == std::string::npos) break;
        at = dash + 1;
    }
    if (!wantShift && !wantCtrl && !wantAlt) return false;
    if (!wantButton.empty()) {
        // Only while a click is being dispatched. Asked at any other moment -
        // an OnUpdate deciding what a tooltip should say - the modifier alone
        // is the whole of the question, and the button has not been pressed.
        const std::string& clicking = currentClickButton();
        if (!clicking.empty() && clicking != wantButton) return false;
    }
    return (!wantShift || shiftHeld()) && (!wantCtrl || ctrlHeld()) &&
           (!wantAlt || altHeld());
}

// IsModifiedClick(action) → boolean
static int lua_IsModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (act.empty()) {
        // No action named means "is this click modified at all", which is how
        // the paperdoll and the bags branch: IsModifiedClick() picks between
        // OnModifiedClick and the plain OnClick. Answering with shift alone
        // sent every ctrl-click down the plain path, so ctrl-clicking a worn
        // item picked it up instead of putting it in the dressing room.
        lua_pushboolean(L, (shiftHeld() || ctrlHeld() || altHeld()) ? 1 : 0);
        return 1;
    }
    const std::string binding = modifiedClickBinding(L, act);
    const bool answer = binding.empty() ? shiftHeld() : modifiersHeldFor(binding);
    // The gate the comparison tooltip goes through, said once each way.
    //
    // Shift-hovering an item that differs from the worn one produced no
    // refusal from SetHyperlinkCompareItem at all, which means FrameXML never
    // got as far as calling it - and this is what it asks first. Whether the
    // answer is yes or no, and which binding decided it, is the difference
    // between a modifier this client never sees and a comparison that is
    // refused further along.
    if (act == "COMPAREITEMS") {
        static bool saidYes = false, saidNo = false;
        bool& said = answer ? saidYes : saidNo;
        if (!said) {
            said = true;
            LOG_WARNING("IsModifiedClick(COMPAREITEMS) = ", answer ? "true" : "false",
                        ", binding '", binding.empty() ? "(none - falls back to shift)" : binding,
                        "', shift held ", shiftHeld() ? "yes" : "no");
        }
    }
    lua_pushboolean(L, answer ? 1 : 0);
    return 1;
}

// GetModifiedClick(action) → the binding it is on ("SHIFT", "CTRL-BUTTON1", "NONE")
static int lua_GetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::string binding = modifiedClickBinding(L, act);
    lua_pushstring(L, binding.empty() ? "SHIFT" : binding.c_str());
    return 1;
}

// SetModifiedClick(action, binding) - rebinding one from the options panel.
//
// It wrote nowhere, so the panel accepted a change and the next question about
// that action answered the old value. The table the emitter fills is the same
// one read above, so writing to it is the whole of it.
static int lua_SetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    const char* binding = luaL_optstring(L, 2, "NONE");
    if (!action || !*action) return 0;
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    lua_getglobal(L, "__WoweeModifiedClick");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    lua_pushstring(L, binding);
    lua_setfield(L, -2, act.c_str());
    lua_pop(L, 1);
    // And the store, so it comes back next time. Bindings.xml rewrites the
    // table with its defaults at every load, and SaveBindings writes keys,
    // not clicks: nothing else kept the self-cast, focus-cast and auto-loot
    // keys past the session they were set in.
    std::string stored = "modifiedclick_" + act;
    for (char& c : stored) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    setStoredCVar(stored, binding);
    return 0;
}


// --- Trading ---
//
// Placing an item is the only part that needs the cursor, which is why these
// live here. The peer's side is read-only: a trade slot of theirs can be looked
// at and not touched, so clicking one does nothing rather than pretending.

static int pushTradeSlot(lua_State* L, game::GameHandler* gh,
                         const game::GameHandler::TradeSlot& slot) {
    if (!slot.occupied() || slot.itemId == 0) { return luaReturnNil(L); }
    const auto* info = gh->getItemInfo(slot.itemId);
    lua_pushstring(L, info ? info->name.c_str() : "");
    lua_pushstring(L, gh->getItemIconPath(
        info && info->displayInfoId ? info->displayInfoId : slot.displayId).c_str());
    lua_pushnumber(L, slot.stackCount);
    lua_pushboolean(L, 1);       // isUsable
    lua_pushnil(L);              // enchantment: not carried by the trade packet
    return 5;
}

static int lua_GetTradePlayerItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeSlot(L, gh, gh->getMyTradeSlots()[i - 1]);
}

static int lua_GetTradeTargetItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeSlot(L, gh, gh->getPeerTradeSlots()[i - 1]);
}

static int pushTradeLink(lua_State* L, game::GameHandler* gh,
                         const game::GameHandler::TradeSlot& slot) {
    const auto* info = (gh && slot.occupied()) ? gh->getItemInfo(slot.itemId) : nullptr;
    if (!info || info->name.empty()) { return luaReturnNil(L); }
    const std::string link = game::itemChatLink(slot.itemId, static_cast<uint32_t>(info->quality), info->name);
    lua_pushstring(L, link.c_str());
    return 1;
}

static int lua_GetTradePlayerItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeLink(L, gh, gh->getMyTradeSlots()[i - 1]);
}

static int lua_GetTradeTargetItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeLink(L, gh, gh->getPeerTradeSlots()[i - 1]);
}

// ClickTradeButton(slot) - put what is held into the slot, or take back what is
// already there when nothing is held
static int lua_ClickTradeButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) return 0;
    uint8_t srcBag = 0, srcSlot = 0;
    // Through the same translation as every other send. This passed FrameXML's
    // own numbering straight to the server - bags counted 0 to 4 and slots from
    // one - where the wire wants the flat space GetItemByPos reads: container
    // 255 with an absolute slot, or a worn bag's equipment slot with a 0-based
    // index inside it. The server finds no item at the slot it was given and
    // answers TRADE_STATUS_TRADE_CANCELED, so offering an item ended the trade.
    if (cursor(L).type == CursorType::ITEM && cursor(L).bag >= 0 &&
        cursorWireSlot(L, srcBag, srcSlot)) {
        gh->setTradeItem(static_cast<uint8_t>(i - 1), srcBag, srcSlot);
        clearCursorItem(L);
    } else {
        gh->clearTradeItem(static_cast<uint8_t>(i - 1));
    }
    return 0;
}

// The other side's slots belong to the other player.
static int lua_ClickTargetTradeButton(lua_State* L) { (void)L; return 0; }

static int lua_SetTradeMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->setTradeGold(static_cast<uint64_t>(luaL_optnumber(L, 1, 0)));
    return 0;
}

static int lua_CloseTrade(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->cancelTrade();
    return 0;
}

static int lua_BeginTrade(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->acceptTradeRequest();
    return 0;
}

// --- Keybinding API ---
//
// Which commands exist, and in what order, comes from bindings.xml: the emitter
// turns it into __WoweeBindings (the list the UI walks, header rows included)
// and __WoweeBindingScripts (what each one runs). What each is *bound to* lives
// here instead, because it outlives any one Lua state and has to reach a file.

namespace {

/// command → its two keys, empty meaning not bound. Two because the UI offers a
/// primary and a secondary and will write either.
std::map<std::string, std::array<std::string, 2>>& bindingKeys() {
    static std::map<std::string, std::array<std::string, 2>> keys;
    return keys;
}

/// Where an older build kept the bindings. Read while the account data has
/// none; never written.
std::string bindingsFilePath() {
    return core::getConfigRoot() + "/bindings.cfg";
}

/// The sets LoadBindings and SaveBindings take, numbered as the interface
/// numbers them (DEFAULT_BINDINGS, ACCOUNT_BINDINGS, CHARACTER_BINDINGS).
constexpr int kDefaultBindings   = 0;
constexpr int kAccountBindings   = 1;
constexpr int kCharacterBindings = 2;

/// ImGui spells keys as "C" and "Space"; the interface expects "C" and "SPACE",
/// and compares them as strings everywhere.
std::string wowKeyFromImGui(ImGuiKey key) {
    // An action with nothing bound to it answers "None", which uppercased is a
    // perfectly ordinary-looking key name and would show as one.
    if (key == ImGuiKey_None) return "";
    const char* name = ImGui::GetKeyName(key);
    if (!name || !*name) return "";
    std::string out(name);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

/// The commands the client has a real action behind. Rebinding one of these has
/// to reach the manager, or the list would show the new key while the client
/// went on answering to the old one.
struct LiveBinding {
    const char* command;
    wowee::ui::KeybindingManager::Action action;
};
const LiveBinding kLiveBindings[] = {
    {"TOGGLECHARACTER0",  wowee::ui::KeybindingManager::Action::TOGGLE_CHARACTER_SCREEN},
    {"TOGGLEBACKPACK",    wowee::ui::KeybindingManager::Action::TOGGLE_BAGS},
    {"TOGGLESPELLBOOK",   wowee::ui::KeybindingManager::Action::TOGGLE_SPELLBOOK},
    {"TOGGLETALENTS",     wowee::ui::KeybindingManager::Action::TOGGLE_TALENTS},
    {"TOGGLEQUESTLOG",    wowee::ui::KeybindingManager::Action::TOGGLE_QUESTS},
    {"TOGGLEWORLDMAP",    wowee::ui::KeybindingManager::Action::TOGGLE_WORLD_MAP},
    {"TOGGLEMINIMAP",     wowee::ui::KeybindingManager::Action::TOGGLE_MINIMAP},
    {"TOGGLEACHIEVEMENT", wowee::ui::KeybindingManager::Action::TOGGLE_ACHIEVEMENTS},
    {"TOGGLEGUILDTAB",    wowee::ui::KeybindingManager::Action::TOGGLE_GUILD_ROSTER},
};

/// The named key whose name matches, or none. ImGui offers no reverse lookup,
/// and the set is small enough that walking it costs nothing next to the file
/// write that follows a rebind.
ImGuiKey imGuiKeyFromWow(const std::string& name) {
    if (name.empty()) return ImGuiKey_None;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (wowKeyFromImGui(static_cast<ImGuiKey>(k)) == name) {
            return static_cast<ImGuiKey>(k);
        }
    }
    return ImGuiKey_None;
}

/// Hand a binding to the client's own keybinding manager, if it answers that
/// command at all.
///
/// The interface allows two keys per command and the manager holds one per
/// action, so what is pushed here is keys[0] - the primary. The second stays
/// in the Lua map above. Both are kept past the session: SaveBindings writes
/// the two slots of every command in that map, and LoadBindings reads them
/// back and pushes the primary here again.
///
/// A binding for a command not in kLiveBindings does not reach the manager at
/// all. That is intended: those are the commands the interface answers for
/// itself.
void pushBindingToClient(const std::string& command, const std::string& key) {
    for (const auto& live : kLiveBindings) {
        if (command != live.command) continue;
        const ImGuiKey imKey = imGuiKeyFromWow(key);
        wowee::ui::KeybindingManager::getInstance().setKeyForAction(live.action, imKey);
        return;
    }
}

/// The key the client is listening for, when this is a command it acts on.
///
/// Read at the moment it is asked rather than from the copy below, because the
/// client's own settings panel can rebind these too - and then the copy is a
/// second answer to a question the manager already owns, one keystroke out of
/// date and shown on every action button.
std::optional<std::string> liveKeyFor(const std::string& command) {
    for (const auto& live : kLiveBindings) {
        if (command != live.command) continue;
        std::string key =
            wowKeyFromImGui(wowee::ui::KeybindingManager::getInstance()
                                .getKeyForAction(live.action));
        if (key.empty()) return std::nullopt;
        return key;
    }
    return std::nullopt;
}

using BindingMap = std::map<std::string, std::array<std::string, 2>>;

BindingMap buildDefaultBindings();

/// What every set of bindings is a difference from - the bindings before any
/// were loaded. Taken once, on first use: the keys the client's own actions
/// answer to are read off its manager, and from then on loading a set moves
/// them, so asking again would take a set's keys for the defaults.
const BindingMap& defaultBindings() {
    static const BindingMap defaults = buildDefaultBindings();
    return defaults;
}

/// Start the bindings over from the defaults, if nothing has yet.
void seedBindingDefaults() {
    if (bindingKeys().empty()) bindingKeys() = defaultBindings();
}

/// The keys the client is actually listening for, asked of the manager that
/// listens rather than restated here - a second copy would be wrong the moment
/// either side moved. Commands the client has no action for keep their retail
/// default, so the list reads correctly even where nothing acts on it yet.
BindingMap buildDefaultBindings() {
    BindingMap keys;

    // The original client's defaults for the keys this one answers. Movement,
    // the number row and its shift and control layers run through these and
    // nothing else, so a key missing here is a key that does nothing until an
    // account binds it.
    static const struct { const char* command; const char* key; const char* second; } kDefaults[] = {
        {"MOVEFORWARD", "W", "UP"},     {"MOVEBACKWARD", "S", "DOWN"},
        {"TURNLEFT", "A", "LEFT"},      {"TURNRIGHT", "D", "RIGHT"},
        {"STRAFELEFT", "Q", ""},        {"STRAFERIGHT", "E", ""},
        {"JUMP", "SPACE", ""},          {"TOGGLEAUTORUN", "NUMLOCK", ""},
        {"SITORSTAND", "X", ""},
        // Tab targets, and has since the client was written; the panel said
        // this one was unbound because nothing had ever told it.
        {"TARGETNEARESTENEMY", "TAB", ""},
        {"TOGGLEGAMEMENU", "ESCAPE", ""},
        {"SCREENSHOT", "PRINTSCREEN", ""},
        {"ACTIONBUTTON1", "1", ""}, {"ACTIONBUTTON2", "2", ""},
        {"ACTIONBUTTON3", "3", ""}, {"ACTIONBUTTON4", "4", ""},
        {"ACTIONBUTTON5", "5", ""}, {"ACTIONBUTTON6", "6", ""},
        {"ACTIONBUTTON7", "7", ""}, {"ACTIONBUTTON8", "8", ""},
        {"ACTIONBUTTON9", "9", ""}, {"ACTIONBUTTON10", "0", ""},
        {"ACTIONBUTTON11", "-", ""},{"ACTIONBUTTON12", "=", ""},
        // Shift and a number turns the main bar's page; control and a number
        // is the pet's bar, control and a function key the stance bar.
        {"ACTIONPAGE1", "SHIFT-1", ""}, {"ACTIONPAGE2", "SHIFT-2", ""},
        {"ACTIONPAGE3", "SHIFT-3", ""}, {"ACTIONPAGE4", "SHIFT-4", ""},
        {"ACTIONPAGE5", "SHIFT-5", ""}, {"ACTIONPAGE6", "SHIFT-6", ""},
        {"BONUSACTIONBUTTON1", "CTRL-1", ""}, {"BONUSACTIONBUTTON2", "CTRL-2", ""},
        {"BONUSACTIONBUTTON3", "CTRL-3", ""}, {"BONUSACTIONBUTTON4", "CTRL-4", ""},
        {"BONUSACTIONBUTTON5", "CTRL-5", ""}, {"BONUSACTIONBUTTON6", "CTRL-6", ""},
        {"BONUSACTIONBUTTON7", "CTRL-7", ""}, {"BONUSACTIONBUTTON8", "CTRL-8", ""},
        {"BONUSACTIONBUTTON9", "CTRL-9", ""}, {"BONUSACTIONBUTTON10", "CTRL-0", ""},
        {"SHAPESHIFTBUTTON1", "CTRL-F1", ""}, {"SHAPESHIFTBUTTON2", "CTRL-F2", ""},
        {"SHAPESHIFTBUTTON3", "CTRL-F3", ""}, {"SHAPESHIFTBUTTON4", "CTRL-F4", ""},
        {"SHAPESHIFTBUTTON5", "CTRL-F5", ""}, {"SHAPESHIFTBUTTON6", "CTRL-F6", ""},
        {"SHAPESHIFTBUTTON7", "CTRL-F7", ""}, {"SHAPESHIFTBUTTON8", "CTRL-F8", ""},
        {"SHAPESHIFTBUTTON9", "CTRL-F9", ""}, {"SHAPESHIFTBUTTON10", "CTRL-F10", ""},
    };
    for (const auto& d : kDefaults) keys[d.command] = {d.key, d.second};

    // Where a command corresponds to something the client really does, the key
    // shown is the one it really answers to.
    auto& manager = wowee::ui::KeybindingManager::getInstance();
    for (const auto& live : kLiveBindings) {
        const std::string key = wowKeyFromImGui(manager.getKeyForAction(live.action));
        if (!key.empty()) keys[live.command] = {key, ""};
    }

    // And the controller's own defaults, as second keys.
    //
    // The pad's scheme was written in C++ and pressed keys directly, so the
    // game's Key Bindings panel showed Action Button 3 on "3" and said
    // nothing about the D-pad: a player who opened the one screen that exists
    // for finding out what a button does saw a controller bound to nothing,
    // and had no way to change any of it.
    //
    // Read off the table that performs the scheme rather than restated here,
    // so the two cannot drift. The rows with no command - the bumper that is
    // only a modifier, the button that raises the pointer - stay the client's
    // own, having nothing in the interface's vocabulary to be.
    //
    // Second rather than first, so the keyboard's key is still the one shown
    // where only one fits. A row's shifted command is the button with the
    // left bumper held, which holds shift.
    const auto seedPad = [&keys](std::span<const wowee::ui::PadBinding> rows) {
        const auto seed = [&keys](const char* command, const std::string& padKey) {
            if (!command || !*command) return;
            auto& pair = keys[command];
            if (pair[0].empty()) pair[0] = padKey;
            else if (pair[1].empty()) pair[1] = padKey;
        };
        for (const wowee::ui::PadBinding& row : rows) {
            const char* padKey = wowee::ui::padKeyName(row.button);
            if (!padKey || !*padKey) continue;
            seed(row.command, padKey);
            seed(row.shiftedCommand, std::string("SHIFT-") + padKey);
        }
    };
    seedPad(wowee::ui::padBindings());
    seedPad(wowee::ui::padExtraBindings());
    return keys;
}

/// Give a key to a command, or to nothing. A key belongs to one command at a
/// time, so it leaves the one that had it before it joins another - otherwise
/// both claim it and which one answers depends on map order.
void bindKey(const std::string& key, const char* command) {
    for (auto& [existing, keys] : bindingKeys()) {
        for (std::string& slot : keys) {
            if (slot != key) continue;
            slot.clear();
            // The command that just lost the key has to be told, not only the
            // one that gained it, or the client answers to both. What it
            // answers to now is whichever slot still holds something, which is
            // not always the first.
            pushBindingToClient(existing, keys[0].empty() ? keys[1] : keys[0]);
        }
    }
    if (command) {
        auto& keys = bindingKeys()[command];
        if (keys[0].empty()) keys[0] = key; else keys[1] = key;
        pushBindingToClient(command, keys[0]);
    }
}

/// Which set is in force: the character's when it has one of its own, the
/// account's otherwise - GetCurrentBindingSet's answer.
int currentBindingSet(game::GameHandler* gh) {
    return (gh && !gh->accountData().text(game::AccountDataType::CharacterBindings).empty())
               ? kCharacterBindings : kAccountBindings;
}

/// The bindings an older build kept in bindings.cfg, one "COMMAND=key1,key2"
/// a line, read over the defaults. Only while the account data holds no
/// bindings at all: the first SaveBindings writes them there.
void readLegacyBindingsFile() {
    std::ifstream in(bindingsFilePath());
    if (!in) return;   // Nothing saved yet is not a failure.
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string command = line.substr(0, eq);
        const std::string rest = line.substr(eq + 1);
        const size_t comma = rest.find(',');
        bindingKeys()[command] = {
            rest.substr(0, comma),
            comma == std::string::npos ? "" : rest.substr(comma + 1)
        };
        pushBindingToClient(command, bindingKeys()[command][0]);
    }
}

/// Put the bindings of one set in force: the defaults, then the set's lines
/// over them in order, as the original client applies bindings-cache.wtf.
void loadBindingSet(game::GameHandler* gh, int which) {
    bindingKeys() = defaultBindings();
    // The client's own actions go back to their defaults too: a key the last
    // set moved and this one does not mention is the default's again.
    for (const auto& live : kLiveBindings) {
        const auto it = defaultBindings().find(live.command);
        pushBindingToClient(live.command, it == defaultBindings().end() ? "" : it->second[0]);
    }
    if (which == kDefaultBindings) return;

    std::string text;
    game::AccountDataType type = game::AccountDataType::GlobalBindings;
    if (gh) {
        if (which == kCharacterBindings) {
            text = gh->accountData().text(game::AccountDataType::CharacterBindings);
            type = game::AccountDataType::CharacterBindings;
        }
        if (text.empty()) {
            text = gh->accountData().text(game::AccountDataType::GlobalBindings);
            type = game::AccountDataType::GlobalBindings;
        }
    }
    if (text.empty()) {
        readLegacyBindingsFile();
        return;
    }
    std::vector<std::string> problems;
    const auto lines = game::parseBindingsCache(text, &problems);
    for (const std::string& problem : problems) {
        LOG_WARNING("Key bindings (", game::accountDataName(type), "): ", problem, "; skipped");
    }
    for (const auto& line : lines) {
        bindKey(line.key, line.command == game::kUnboundCommand ? nullptr : line.command.c_str());
    }
    LOG_INFO("Key bindings: ", lines.size(), " lines of ", game::accountDataName(type), " applied");
}

/// The bindings in force, as the lines that make them from the defaults: each
/// key bound other than by default, and NONE for each default key now bound to
/// nothing. The original client writes the same difference, so either reads
/// what the other saved.
std::string writeBindingSet() {
    const BindingMap& defaults = defaultBindings();
    std::map<std::string, std::string> now;  // key -> command
    for (const auto& [command, keys] : bindingKeys()) {
        for (const std::string& key : keys) {
            if (!key.empty()) now[key] = command;
        }
    }
    std::vector<game::KeyBindingLine> lines;
    for (const auto& [command, keys] : defaults) {
        for (const std::string& key : keys) {
            if (!key.empty() && !now.count(key)) lines.push_back({key, game::kUnboundCommand});
        }
    }
    for (const auto& [key, command] : now) {
        const auto it = defaults.find(command);
        const bool isDefault = it != defaults.end() &&
                               (it->second[0] == key || it->second[1] == key);
        if (!isDefault) lines.push_back({key, command});
    }
    return game::writeBindingsCache(lines);
}

/// The command list the emitter built, or zero if bindings.xml never loaded.
int bindingCount(lua_State* L) {
    lua_getglobal(L, "__WoweeBindings");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    const int n = static_cast<int>(lua_objlen(L, -1));
    lua_pop(L, 1);
    return n;
}

/// The command at a one-based position, empty when out of range.
std::string bindingAt(lua_State* L, int index) {
    lua_getglobal(L, "__WoweeBindings");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return ""; }
    lua_rawgeti(L, -1, index);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "";
    lua_pop(L, 2);
    return out;
}

}  // namespace

bool clientActsOnBinding(const std::string& command) {
    // The nine the client really performs, asked of the same list that keeps
    // their keys in step rather than restated - a second copy would be wrong
    // the moment either side moved.
    for (const auto& live : kLiveBindings) {
        if (command == live.command) return true;
    }
    // And the rest of what the client answers without going through a binding
    // at all. Each is a real call site: chat opens on Enter and on slash, Tab
    // targets, Print Screen and Z are read in GameScreen, and Escape has a
    // whole chain of its own in escape_action.hpp.
    //
    // Movement and the action bar are not here: bindings.xml drives them, as
    // it does in the original client - MoveForwardStart on the press of
    // whatever key MOVEFORWARD is on, ActionButtonDown and ActionButtonUp for
    // ACTIONBUTTON1 - so a key the account moves them to is the key they
    // answer.
    //
    // By command rather than by key, because the conflict is that the *action*
    // happens twice. Most bindings toggle something, so two answers to one
    // press cancel out and the key reads as dead - which is worse than doing
    // nothing, since it looks like the binding never ran.
    static constexpr const char* kClientAnswers[] = {
        "TOGGLEGAMEMENU", "OPENCHAT", "OPENCHATSLASH", "TARGETNEARESTENEMY",
        "SCREENSHOT", "TOGGLESHEATH",
    };
    for (const char* name : kClientAnswers) {
        if (command == name) return true;
    }
    return false;
}

int clientImGuiKeyForBinding(const std::string& command) {
    for (const auto& live : kLiveBindings) {
        if (command != live.command) continue;
        return static_cast<int>(
            wowee::ui::KeybindingManager::getInstance().getKeyForAction(live.action));
    }
    return 0;
}

// GetBindingKey(command) → key1, key2 (or nil)
static int lua_GetBindingKey(lua_State* L) {
    seedBindingDefaults();
    const std::string command = luaL_checkstring(L, 1);
    if (auto live = liveKeyFor(command)) {
        lua_pushstring(L, live->c_str());
        lua_pushnil(L);
        return 2;
    }
    auto it = bindingKeys().find(command);
    if (it == bindingKeys().end()) { lua_pushnil(L); lua_pushnil(L); return 2; }
    for (const std::string& key : it->second) {
        if (key.empty()) lua_pushnil(L); else lua_pushstring(L, key.c_str());
    }
    return 2;
}

// GetBindingAction(key) → command, or "" for a key that holds nothing
//
// Empty rather than nil, which is what the real client answers and what the
// interface is written against. Blizzard's key binding panel reads it as
//
//     local oldAction = GetBindingAction(keyPressed, KeyBindingFrame.mode);
//     if ( oldAction ~= "" and oldAction ~= KeyBindingFrame.selected ) then
//         local key1, key2 = GetBindingKey(oldAction, ...)
//
// and nil is not equal to "", so a nil sent the panel into that branch with
// nil in hand: "bad argument #1 to 'GetBindingKey' (string expected, got
// nil)", thrown out of OnKeyDown before the new binding was ever set. Every
// attempt to bind a key that was not already bound to something failed that
// way - a controller button being the case where that is always true.
static int lua_GetBindingAction(lua_State* L) {
    seedBindingDefaults();
    const std::string key = luaL_checkstring(L, 1);
    for (const auto& [command, keys] : bindingKeys()) {
        if (keys[0] == key || keys[1] == key) {
            lua_pushstring(L, command.c_str());
            return 1;
        }
    }
    lua_pushstring(L, "");
    return 1;
}

// GetNumBindings() → how many rows the list has, headers included
static int lua_GetNumBindings(lua_State* L) {
    lua_pushinteger(L, bindingCount(L));
    return 1;
}

// GetBinding(index) → command, key1, key2. A header row is a command like
// "HEADER_MOVEMENT" with no keys, which is how the UI tells the two apart.
static int lua_GetBinding(lua_State* L) {
    seedBindingDefaults();
    const int index = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string command = bindingAt(L, index);
    if (command.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, command.c_str());
    if (auto live = liveKeyFor(command)) {
        lua_pushstring(L, live->c_str());
        lua_pushnil(L);
        return 3;
    }
    auto it = bindingKeys().find(command);
    if (it == bindingKeys().end()) { lua_pushnil(L); lua_pushnil(L); return 3; }
    for (const std::string& key : it->second) {
        if (key.empty()) lua_pushnil(L); else lua_pushstring(L, key.c_str());
    }
    return 3;
}

// SetBinding(key, command) → true. A nil command clears whatever holds the key,
// which is how the UI unbinds.
static int lua_SetBinding(lua_State* L) {
    seedBindingDefaults();
    const std::string key = luaL_checkstring(L, 1);
    const char* command = lua_isnoneornil(L, 2) ? nullptr : luaL_checkstring(L, 2);
    bindKey(key, command);
    // Six frames wait on this, the action buttons among them: it is what
    // redraws the little key printed in the corner. Without it a rebind takes
    // effect while every button goes on showing the old key.
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    lua_pushboolean(L, 1);
    return 1;
}

// SaveBindings(which) - keep what is bound where, as the account's set (1) or
// the character's (2).
//
// Into the account data, which the server keeps and every client of the
// account reads - the same bindings-cache.wtf text the original client writes.
// Saving the account's set ends the character's, as it does there: unticking
// "Character Specific Key Bindings" is how a player goes back to the account's.
static int lua_SaveBindings(lua_State* L) {
    seedBindingDefaults();
    auto* gh = getGameHandler(L);
    if (!gh) {
        LOG_WARNING("SaveBindings: no game session to keep the key bindings in; "
                    "they last until the client closes");
        return 0;
    }
    const int which = static_cast<int>(luaL_optnumber(L, 1, kAccountBindings));
    if (which != kAccountBindings && which != kCharacterBindings) {
        LOG_WARNING("SaveBindings(", which, "): not a set bindings are saved to; nothing saved");
        return 0;
    }
    auto& data = gh->accountData();
    const std::string text = writeBindingSet();
    if (which == kCharacterBindings) {
        data.store(game::AccountDataType::CharacterBindings, text);
    } else {
        data.store(game::AccountDataType::GlobalBindings, text);
        if (!data.text(game::AccountDataType::CharacterBindings).empty()) {
            data.store(game::AccountDataType::CharacterBindings, "");
        }
    }
    return 0;
}

// LoadBindings(which) - put a set in force: the defaults (0), the account's
// (1), or the character's (2, the account's when it has none of its own).
static int lua_LoadBindings(lua_State* L) {
    const int which = static_cast<int>(luaL_optnumber(L, 1, kAccountBindings));
    loadBindingSet(getGameHandler(L), which);
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    return 0;
}

// RunBinding(command, keystate) → runs what bindings.xml says the command does
static int lua_RunBinding(lua_State* L) {
    const std::string command = luaL_checkstring(L, 1);
    const char* keystate = lua_isnoneornil(L, 2) ? "down" : luaL_checkstring(L, 2);
    runBindingCommand(L, command, std::strcmp(keystate, "up") != 0);
    return 0;
}

namespace {

/// Call a global function with one string, saying so when it fails.
bool callBindingTarget(lua_State* L, const std::string& command, const char* function,
                       const std::string& arg) {
    lua_getglobal(L, function);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        LOG_WARNING("Key binding ", command, ": ", function,
                    " is not defined in this interface; the key does nothing");
        return false;
    }
    lua_pushstring(L, arg.c_str());
    if (checkedLuaCall(L, 1, 0, 0, "key binding") != 0) {
        LOG_WARNING("Key binding ", command, " failed: ", luaL_optstring(L, -1, "?"));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

/// "CLICK frame:button": the frame clicked with that button, LeftButton when
/// none is named.
bool clickBindingTarget(lua_State* L, const std::string& command, const std::string& target) {
    const size_t colon = target.find(':');
    const std::string frame = target.substr(0, colon);
    const std::string button = colon == std::string::npos ? "LeftButton" : target.substr(colon + 1);
    lua_getglobal(L, frame.c_str());
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        LOG_WARNING("Key binding ", command, ": there is no frame ", frame,
                    " to click; the key does nothing");
        return false;
    }
    lua_getfield(L, -1, "Click");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        LOG_WARNING("Key binding ", command, ": ", frame,
                    " cannot be clicked; the key does nothing");
        return false;
    }
    lua_pushvalue(L, -2);
    lua_pushstring(L, button.c_str());
    if (checkedLuaCall(L, 2, 0, 0, "key binding") != 0) {
        LOG_WARNING("Key binding ", command, " failed: ", luaL_optstring(L, -1, "?"));
        lua_pop(L, 2);
        return false;
    }
    lua_pop(L, 1);
    return true;
}

} // namespace

bool bindingRunsOnUp(lua_State* L, const std::string& command) {
    lua_getglobal(L, "__WoweeBindingRunOnUp");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
    lua_getfield(L, -1, command.c_str());
    const bool onUp = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return onUp;
}

bool runBindingCommand(lua_State* L, const std::string& command, bool down) {
    // A key bound straight to a spell, a macro, an item or a button. What the
    // original client writes to bindings-cache.wtf for SetBindingSpell and
    // the rest, so an account that has them brings them here.
    static constexpr struct { const char* prefix; const char* function; } kTargets[] = {
        {"SPELL ", "CastSpellByName"},
        {"MACRO ", "RunMacro"},
        {"ITEM ",  "UseItemByName"},
    };
    for (const auto& target : kTargets) {
        const size_t n = std::strlen(target.prefix);
        if (command.compare(0, n, target.prefix) != 0) continue;
        return !down || callBindingTarget(L, command, target.function, command.substr(n));
    }
    if (command.compare(0, 6, "CLICK ") == 0) {
        return !down || clickBindingTarget(L, command, command.substr(6));
    }

    lua_getglobal(L, "__WoweeBindingScripts");
    if (lua_istable(L, -1)) lua_getfield(L, -1, command.c_str()); else lua_pushnil(L);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        static std::set<std::string> said;
        if (said.insert(command).second) {
            LOG_WARNING("Key binding ", command, ": nothing in the interface runs it; "
                        "the key does nothing");
        }
        return false;
    }
    lua_pushstring(L, down ? "down" : "up");
    if (checkedLuaCall(L, 1, 0, 0, "key binding") != 0) {
        LOG_WARNING("Key binding ", command, " failed: ", luaL_optstring(L, -1, "?"));
        lua_pop(L, 2);
        return false;
    }
    lua_pop(L, 1);
    return true;
}
// The override-binding family, none of which this client implements - only
// the Click variant was ever *bound*, and the other four raised. They are
// reached from restrictedframes.lua, the secure-frame machinery behind action
// buttons and unit frames, so a raise there breaks the frame rather than the
// binding. Bound alongside it, doing the same nothing.
static int lua_SetOverrideBindingClick(lua_State* L) { (void)L; return 0; }
static int lua_SetOverrideBinding(lua_State* L) { (void)L; return 0; }
static int lua_SetOverrideBindingSpell(lua_State* L) { (void)L; return 0; }
static int lua_SetOverrideBindingMacro(lua_State* L) { (void)L; return 0; }
static int lua_SetOverrideBindingItem(lua_State* L) { (void)L; return 0; }
static int lua_ClearOverrideBindings(lua_State* L) { (void)L; return 0; }

// Frame methods: SetPoint, SetSize, SetWidth, SetHeight, GetWidth, GetHeight, GetCenter, SetAlpha, GetAlpha

void registerActionLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"HasAction",           lua_HasAction},
                // A macro's name, which only a macro has; ActionButton_Update
                // shows it under the icon and expects nothing for a spell.
                // GetActionText(slot) → the name written under the button.
                //
                // Only a macro has one, which is what the action button draws
                // in its Name font string - so with nil answered for every slot
                // a bar full of macros was a row of nameless icons, all of them
                // the same question mark unless the player had picked an icon.
                // The names were there: GetMacroInfo has been reading them off
                // the same handler all along.
                {"GetActionText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushnil(L); return 1; }
            const auto& bar = gh->getActionBar();
            if (slot >= static_cast<int>(bar.size())) { lua_pushnil(L); return 1; }
            const auto& action = bar[static_cast<size_t>(slot)];
            if (action.type != game::ActionBarSlot::MACRO) { lua_pushnil(L); return 1; }
            const auto* macro = gh->accountData().macro(action.id);
            if (!macro || macro->name.empty()) { lua_pushnil(L); return 1; }
            lua_pushstring(L, macro->name.c_str());
            return 1;
        }},
                // Whether the action has a range to be in or out of. Nothing
                // here tracks that yet, and false is what "no range check"
                // looks like - the button simply never dims for distance.
                {"ActionHasRange",      [](lua_State* L) -> int {
                    lua_pushboolean(L, 0);
                    return 1;
                }},
                {"GetActionTexture",    lua_GetActionTexture},
                {"IsCurrentAction",     lua_IsCurrentAction},
                {"IsUsableAction",      lua_IsUsableAction},
                {"IsActionInRange",     lua_IsActionInRange},
                {"GetActionInfo",       lua_GetActionInfo},
                {"GetActionCount",      lua_GetActionCount},
                {"GetActionCooldown",   lua_GetActionCooldown},
                {"UseAction",           lua_UseAction},
                {"PickupAction",        lua_PickupAction},
                {"PlaceAction",         lua_PlaceAction},
                {"PickupSpell",         lua_PickupSpell},
                {"PickupCompanion",     lua_PickupCompanion},
                {"PickupGuildBankItem", lua_PickupGuildBankItem},
                {"PlaceGlyphInSocket",  lua_PlaceGlyphInSocket},
                {"SplitGuildBankItem",  lua_SplitGuildBankItem},
                {"PickupSpellBookItem", lua_PickupSpellBookItem},
                {"PickupContainerItem", lua_PickupContainerItem},
                {"PickupItem",          lua_PickupItem},
                // The totem bar's summon flyout calls these two from its
                // OnClick and OnLeave, and nothing in this interface defines
                // them - not multicastactionbarframe.lua beside the XML that
                // calls them, not anywhere. An upstream gap rather than one of
                // this client's, but the raise lands here all the same, and it
                // lands on a shaman clicking their own totem bar.
                //
                // Answered with nothing, which makes the button inert. There
                // is no honest alternative: the missing function is what would
                // have decided which totem the flyout chose, and inventing one
                // would summon whatever this guessed.
                {"MultiCastSummonSpellButtonFlyoutButton_OnClick", lua_ReturnNothing2},
                {"MultiCastSummonSpellButtonFlyoutButton_OnLeave", lua_ReturnNothing2},
                {"PickupInventoryItem", lua_PickupInventoryItem},
                {"PickupBagFromSlot",   lua_PickupBagFromSlot},
                {"ClickSendMailItemButton", lua_ClickSendMailItemButton},
                {"ClearCursor",         lua_ClearCursor},
                {"GetCursorInfo",       lua_GetCursorInfo},
                {"PickupPetAction",     lua_PickupPetAction},
                {"CursorHasItem",       lua_CursorHasItem},
                {"CursorHasSpell",      lua_CursorHasSpell},
                {"DeleteCursorItem",    lua_DeleteCursorItem},
                {"AutoEquipCursorItem", lua_AutoEquipCursorItem},
                {"IsModifiedClick",     lua_IsModifiedClick},
                {"GetModifiedClick",    lua_GetModifiedClick},
                {"SetModifiedClick",    lua_SetModifiedClick},
                // Macros - the account data's, two halves of them.
                //
                // The interface numbers them as the macro frame lists them:
                // the account's 1-36 and the character's 37-54, each half by
                // name. That number is an index, not the macro's id - the id
                // is what an action button and the account data carry, and it
                // does not move when a macro is renamed or one before it is
                // deleted. See macroAtIndex.
                {"GetNumMacros",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<double>(gh->accountData().macros(false).size()) : 0.0);
            lua_pushnumber(L, gh ? static_cast<double>(gh->accountData().macros(true).size()) : 0.0);
            return 2;
        }},
                // GetMacroInfo(index or name) → name, texture, body, isLocal
                {"GetMacroInfo",        [](lua_State* L) -> int {
            const game::Macro* macro = macroFromArg(L, 1);
            if (!macro) return luaReturnNil(L);
            lua_pushstring(L, macro->name.c_str());
            lua_pushstring(L, macroTexture(*macro).c_str());
            lua_pushstring(L, macro->body.c_str());
            lua_pushnil(L);
            return 4;
        }},
                // GetMacroBody(index or name) → body
                {"GetMacroBody",        [](lua_State* L) -> int {
            const game::Macro* macro = macroFromArg(L, 1);
            if (!macro) return luaReturnNil(L);
            lua_pushstring(L, macro->body.c_str());
            return 1;
        }},
                // GetMacroIndexByName(name) → index, or 0
                {"GetMacroIndexByName", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const game::Macro* macro = macroFromArg(L, 1);
            lua_pushnumber(L, (gh && macro) ? macroIndexOf(gh, macro->id) : 0);
            return 1;
        }},
                // CreateMacro(name, icon, body, perCharacter) → index
                //
                // The icon is what the macro frame's picker hands over: a
                // position in GetMacroIconInfo's list. A texture path is taken
                // too, as addons pass one.
                {"CreateMacro",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return luaReturnNil(L);
            const char* name = luaL_optstring(L, 1, "");
            const std::string icon = macroIconArg(L, 2);
            const char* body = luaL_optstring(L, 3, "");
            const bool perChar = lua_toboolean(L, 4) != 0;
            const uint32_t id = gh->accountData().createMacro(
                perChar, name, icon.empty() ? kQuestionMarkIcon : icon, body);
            if (id == 0) return luaReturnNil(L);
            lua_pushnumber(L, macroIndexOf(gh, id));
            return 1;
        }},
                // EditMacro(index, name, icon, body) → index
                //
                // Each part is optional and nil means "leave it": the frame
                // renames with only a name and saves the text with only a body.
                // The index that comes back is the macro's new place, which a
                // rename can change.
                {"EditMacro",           [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const game::Macro* macro = macroFromArg(L, 1);
            if (!gh || !macro) return luaReturnNil(L);
            const uint32_t id = macro->id;
            std::optional<std::string> name, icon, body;
            if (lua_isstring(L, 2)) name = lua_tostring(L, 2);
            if (!lua_isnoneornil(L, 3)) icon = macroIconArg(L, 3);
            if (lua_isstring(L, 4)) body = lua_tostring(L, 4);
            gh->accountData().editMacro(id, name ? &*name : nullptr,
                                        (icon && !icon->empty()) ? &*icon : nullptr,
                                        body ? &*body : nullptr);
            lua_pushnumber(L, macroIndexOf(gh, id));
            return 1;
        }},
                // DeleteMacro(index or name) - the ones after it move up a
                // place, as they do in the frame, and the buttons holding it
                // are emptied.
                {"DeleteMacro",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const game::Macro* macro = macroFromArg(L, 1);
            if (gh && macro) gh->accountData().deleteMacro(macro->id);
            return 0;
        }},
                // Saved as each change is made, so this has nothing to do.
                {"SaveMacros",          [](lua_State* L) -> int { (void)L; return 0; }},
                // PickupMacro(index or name) - onto the cursor, so it can be
                // dropped on an action slot. What the cursor holds is the
                // macro's id, which is what the slot will.
                {"PickupMacro",         [](lua_State* L) -> int {
            const game::Macro* macro = macroFromArg(L, 1);
            if (!macro) return 0;
            setCursorType(L, CursorType::MACRO);
            cursor(L).id = macro->id;
            wowee::ui::frameXmlSetCursorItem(macroTexture(*macro));
            return 0;
        }},
                {"GetTradePlayerItemInfo", lua_GetTradePlayerItemInfo},
                {"GetTradeTargetItemInfo", lua_GetTradeTargetItemInfo},
                {"GetTradePlayerItemLink", lua_GetTradePlayerItemLink},
                {"GetTradeTargetItemLink", lua_GetTradeTargetItemLink},
                {"ClickTradeButton",    lua_ClickTradeButton},
                {"ClickTargetTradeButton", lua_ClickTargetTradeButton},
                {"SetTradeMoney",       lua_SetTradeMoney},
                // CancelTradeAccept() - take an acceptance back without
                // closing the trade. The cancel button does this instead of
                // closing while the player has already accepted, so without it
                // the only way out of an accepted trade was to end it.
                {"CancelTradeAccept", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->unacceptTrade();
            return 0;
        }},
                {"CloseTrade",          lua_CloseTrade},
                {"BeginTrade",          lua_BeginTrade},
                {"GetBindingKey",       lua_GetBindingKey},
                {"GetBindingAction",    lua_GetBindingAction},
                {"GetNumBindings",      lua_GetNumBindings},
                {"GetBinding",          lua_GetBinding},
                {"SetBinding",          lua_SetBinding},
                // GetBindingByKey(key) - the binding a key runs, or nil.
                //
                // Reached from GetBindingFromClick, which StaticPopup_OnKeyDown
                // calls on *every* keypress while a popup is up - so with
                // dialogs handed over, pressing any key with one on screen hit
                // an unbound global and raised. Escape is how a popup is
                // dismissed, so it was the keypress most likely to hit it.
                //
                // Two names are all FrameXML compares against here, and both
                // are fixed in this client: escape opens the game menu and
                // print screen takes a screenshot, neither rebindable. Anything
                // else answers nil, which is what WoW answers for an unbound
                // key and which lets the dialog's own key handling run.
                {"GetBindingByKey", [](lua_State* L) -> int {
            std::string key(luaL_optstring(L, 1, ""));
            for (char& c : key) c = static_cast<char>(std::toupper(
                static_cast<unsigned char>(c)));
            if (key == "ESCAPE")           lua_pushstring(L, "TOGGLEGAMEMENU");
            else if (key == "PRINTSCREEN") lua_pushstring(L, "SCREENSHOT");
            else                           lua_pushnil(L);
            return 1;
        }},
                // GetQuestGreenRange() - how many levels below the player a
                // thing can be before it turns grey.
                //
                // This was left unbound on the grounds that a guessed constant
                // would mis-colour every quest and that nothing live called it.
                // The second half was wrong: TargetFrame_CheckLevel reaches it
                // through GetQuestDifficultyColor for any target the player can
                // attack, so it raised before levelText:Show() and an enemy's
                // level never appeared. A friendly target took the other branch
                // and showed its level, which is why it looked like enemies
                // specifically.
                //
                // No guess needed either. AzerothCore's Formulas.h gives the
                // grey level exactly, and the green range is the distance from
                // the player's level down to it.
                {"GetQuestGreenRange", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int lvl = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
            int gray;
            if (lvl <= 5)       gray = 0;
            else if (lvl <= 39) gray = lvl - 5 - lvl / 10;
            else if (lvl <= 59) gray = lvl - 1 - lvl / 5;
            else                gray = lvl - 9;
            lua_pushnumber(L, lvl - gray);
            return 1;
        }},
                // Two latent raises on the escape-key path, bound as no-ops
                // because doing nothing is the right behaviour rather than a
                // placeholder for it. SetUIVisibility is reached only from
                // ToggleGameMenu's first branch, which runs when UIParent is
                // hidden - out of the way until someone hides the interface,
                // and then on the only path that brings it back.
                // ConsoleAddMessage is the sink for FrameXML's debug print.
                {"SetUIVisibility",   [](lua_State* L) -> int { (void)L; return 0; }},
                {"ConsoleAddMessage", [](lua_State* L) -> int { (void)L; return 0; }},
                // GetCurrentBindingSet() - which set SaveBindings should write.
                //
                // 1 is account-wide and 2 is per character: the character's
                // when its account data holds a set of its own. The key
                // binding frame ticks "Character Specific Key Bindings" from
                // this, and interfaceoptionspanels.lua calls
                // SaveBindings(GetCurrentBindingSet()) from three option
                // handlers.
                {"GetCurrentBindingSet", [](lua_State* L) -> int {
            lua_pushnumber(L, currentBindingSet(getGameHandler(L))); return 1;
        }},
                {"SaveBindings",        lua_SaveBindings},
                {"LoadBindings",        lua_LoadBindings},
                {"RunBinding",          lua_RunBinding},
                {"SetOverrideBindingClick", lua_SetOverrideBindingClick},
                {"SetOverrideBinding",      lua_SetOverrideBinding},
                {"SetOverrideBindingSpell", lua_SetOverrideBindingSpell},
                {"SetOverrideBindingMacro", lua_SetOverrideBindingMacro},
                {"SetOverrideBindingItem",  lua_SetOverrideBindingItem},
                {"ClearOverrideBindings", lua_ClearOverrideBindings},
                // Paging lives here, and the getter with it. Both were
                // defined twice - this pair against __WoweeActionBarPage and a
                // bootstrap pair against a local - so whichever won,
                // ChangeActionBarPage could move one number while
                // GetActionBarPage read the other. Removing only the getter
                // made that worse rather than better, which is what happened
                // an hour ago.
                {"GetActionBarPage", [](lua_State* L) -> int {
            lua_getglobal(L, "__WoweeActionBarPage");
            if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 1); }
            return 1;
        }},
                {"ChangeActionBarPage", [](lua_State* L) -> int {
            int page = static_cast<int>(luaL_checknumber(L, 1));
            if (page < 1) page = 1;
            if (page > 6) page = 6;
            lua_pushnumber(L, page);
            lua_setglobal(L, "__WoweeActionBarPage");
            // Through the engine, which delivers to both registries.
            //
            // This walked __WoweeEvents by hand and stopped there. That table
            // is where an addon's RegisterEvent lands; FrameXML registers
            // through frame:RegisterEvent, which fills __WoweeFrameEvents - so
            // the six frames that listen for this, including the one in
            // actionbutton.lua that redraws every button, were never told.
            //
            // The page number moved and nothing else did, which is exactly
            // what the arrows beside the bar looked like: they clicked, they
            // played their sound, and the icons stayed where they were.
            lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
            auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (engine) engine->fireEvent("ACTIONBAR_PAGE_CHANGED", {});
            return 0;
        }},
                // Two returns: whether there is a pet interface at all, and
                // whether it is a hunter's. PetFrame_SetHappiness reads the
                // second to decide whether to draw the happiness icon, and one
                // return left it nil.
                {"HasPetUI", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const bool has = gh && gh->hasPet();
            lua_pushboolean(L, has ? 1 : 0);
            lua_pushboolean(L, 0);   // hunter pet: not distinguished yet
            return 2;
        }},
                // Happiness, for a hunter's pet only. Nil is the honest answer
                // for everyone else and the one PetFrame_SetHappiness guards
                // for - the fallback answering with an object instead made
                // that guard pass and the branch index nothing.
                // GetMirrorTimerInfo(index) → timer, value, maxvalue, scale,
                // paused, label.
                //
                // "UNKNOWN" for a timer that is not running, which is exactly
                // what mirrortimer.lua tests for before using the rest. Absent,
                // the fallback answered with an object, that test passed, and
                // the next line divided a nil by a thousand.
                {"GetMirrorTimerInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
            // Shared with the packet handler that fires MIRROR_TIMER_START, so
            // that polling and event agree on the name MirrorTimer_Hide will
            // later be asked to match.
            const auto& kNames  = game::GameHandler::kMirrorTimerNames;
            const auto& kLabels = game::GameHandler::kMirrorTimerLabels;
            if (!gh || index < 0 || index > 2 || !gh->getMirrorTimer(index).active) {
                lua_pushstring(L, "UNKNOWN");
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
                lua_pushboolean(L, 0);
                lua_pushstring(L, "");
                return 6;
            }
            const auto& t = gh->getMirrorTimer(index);
            lua_pushstring(L, kNames[index]);
            lua_pushnumber(L, t.value);
            lua_pushnumber(L, t.maxValue);
            lua_pushnumber(L, t.scale);
            lua_pushboolean(L, t.paused ? 1 : 0);
            lua_pushstring(L, kLabels[index]);
            return 6;
        }},
                // GetPetHappiness() → happiness 1..3, and what it does to damage
                //
                // Both values, and that is not tidiness. The pet frame does
                // format(PET_DAMAGE_PERCENTAGE, damagePercentage) - "Causes %d%%
                // of normal damage" - the moment happiness is anything at all,
                // and %d against nil raises. Answering nil, as this did before,
                // was safe only because the line above it returns early on a nil
                // happiness; filling in the first value alone would have turned
                // a blank indicator into a dead pet frame for every hunter.
                //
                // Happiness is a power like mana, at index 4, running from
                // nothing to its own maximum. The three faces are the thirds of
                // that range, and the damage each is worth - three quarters,
                // normal, a quarter more - is the game's, not a guess.
                {"GetPetHappiness", [](lua_State* L) -> int {
            auto* pet = resolveUnit(L, "pet");
            if (!pet) { lua_pushnil(L); return 1; }
            const uint32_t maxHappiness = pet->getMaxPowerByType(4);
            if (maxHappiness == 0) { lua_pushnil(L); return 1; }

            const uint32_t happiness = pet->getPowerByType(4);
            const int level = (happiness * 3 >= maxHappiness * 2) ? 3
                            : (happiness * 3 >= maxHappiness)     ? 2
                                                                  : 1;
            static const int kDamagePercent[4] = {0, 75, 100, 125};
            lua_pushnumber(L, level);
            lua_pushnumber(L, kDamagePercent[level]);
            return 2;
        }},
                // Only referenced from commented-out code in 3.3.5, but a
                // temporary pet's timer is nil when there is no timer.
                {"GetPetTimeRemaining", [](lua_State* L) -> int {
            lua_pushnil(L);
            return 1;
        }},
                // GetPetActionSlotUsable(index) → whether the pet can use it
                //
                // Read as "if usable then draw it normally, else grey it out",
                // so an absent answer greyed out every ability the pet has. A
                // slot holding a spell is usable; an empty one has nothing to
                // grey.
                // CancelItemTempEnchantment(hand) - drop a weapon imbue
                //
                // The interface counts the hands from one and the request from
                // zero, which is the only thing between them.
                {"CancelItemTempEnchantment", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int hand = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (gh && (hand == 1 || hand == 2)) {
                gh->cancelTempEnchantment(static_cast<uint8_t>(hand - 1));
            }
            return 0;
        }},
                {"GetPetActionSlotUsable", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const uint32_t spellId = gh->getPetActionSlot(index - 1) & 0x00FFFFFF;
            lua_pushboolean(L, spellId != 0 ? 1 : 0);
            return 1;
        }},
                // GetPetActionInfo(index) →
                //   name, subtext, texture, isToken, isActive,
                //   autoCastAllowed, autoCastEnabled
                //
                // Six of the pet bar's ten slots are not spells at all: three
                // commands (attack, follow, stay) and three stances. This read
                // every slot as a spell, so the low 24 bits - which for those
                // six are 0, 1 or 2 - were looked up as spell ids. The bar's
                // command buttons were drawn with whatever Spell.dbc has at
                // those numbers, or with "Unknown" and a question mark.
                //
                // isToken says which kind a slot is, and PetActionBar_Update
                // reads name and texture as *global names* when it is set:
                //     petActionIcon:SetTexture(_G[texture])
                // Answering a flat false meant even a correct path would have
                // been used the wrong way round.
                {"GetPetActionInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                return luaReturnNil(L);
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            if (packed == 0) { return luaReturnNil(L); }

            const uint32_t action = game::pet::petActionId(packed);
            const auto type = game::pet::petActionType(packed);

            const char* tokenName = nullptr;
            const char* tokenTexture = nullptr;
            bool active = false;

            if (type == game::pet::ActionType::Command) {
                switch (action) {
                    case game::pet::kStay:
                        tokenName = "PET_ACTION_WAIT";    tokenTexture = "PET_WAIT_TEXTURE";   break;
                    case game::pet::kFollow:
                        tokenName = "PET_ACTION_FOLLOW";  tokenTexture = "PET_FOLLOW_TEXTURE"; break;
                    case game::pet::kAttack:
                        tokenName = "PET_ACTION_ATTACK";  tokenTexture = "PET_ATTACK_TEXTURE"; break;
                    case game::pet::kAbandon:
                        tokenName = "PET_DISMISS";        tokenTexture = "PET_DISMISS_TEXTURE"; break;
                    default: break;
                }
            } else if (type == game::pet::ActionType::Reaction) {
                switch (action) {
                    case game::pet::kPassive:
                        tokenName = "PET_MODE_PASSIVE";    tokenTexture = "PET_PASSIVE_TEXTURE";    break;
                    case game::pet::kDefensive:
                        tokenName = "PET_MODE_DEFENSIVE";  tokenTexture = "PET_DEFENSIVE_TEXTURE";  break;
                    case game::pet::kAggressive:
                        tokenName = "PET_MODE_AGGRESSIVE"; tokenTexture = "PET_AGGRESSIVE_TEXTURE"; break;
                    default: break;
                }
                // The stance the pet is in is the one drawn as chosen.
                active = (action == gh->getPetReact());
            }

            if (tokenName) {
                lua_pushstring(L, tokenName);       // 1: name - a global's name
                lua_pushstring(L, "");              // 2: subtext
                lua_pushstring(L, tokenTexture);    // 3: texture - likewise
                lua_pushboolean(L, 1);              // 4: isToken
                lua_pushboolean(L, active ? 1 : 0); // 5: isActive
                // A command or a stance has nothing to cast automatically, and
                // saying otherwise puts the autocast ring on all six of them.
                lua_pushboolean(L, 0);              // 6: autoCastAllowed
                lua_pushboolean(L, 0);              // 7: autoCastEnabled
                return 7;
            }

            const uint32_t spellId = action;
            if (spellId == 0) { return luaReturnNil(L); }
            const std::string& name = gh->getSpellName(spellId);
            const std::string iconPath = gh->getSpellIconPath(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, "");
            lua_pushstring(L, iconPath.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark"
                                               : iconPath.c_str());
            lua_pushboolean(L, 0);                                      // isToken
            // A pet spell is never "active" - that is the checked ring, which
            // belongs to the stance. Reading the autocast bits as active put
            // it on every spell the pet knows.
            lua_pushboolean(L, 0);                                      // isActive
            lua_pushboolean(L, 1);                                      // autoCastAllowed
            lua_pushboolean(L, gh->isPetSpellAutocast(spellId) ? 1 : 0);
            return 7;
        }},
                // IsPetAttackAction(index) - which slot is the attack command,
                // so the bar can flash it while the pet is on a target.
                {"IsPetAttackAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            bool isAttack = false;
            if (gh && index >= 1 && index <= game::GameHandler::PET_ACTION_BAR_SLOTS) {
                const uint32_t packed = gh->getPetActionSlot(index - 1);
                isAttack = game::pet::petActionType(packed) == game::pet::ActionType::Command &&
                           game::pet::petActionId(packed) == game::pet::kAttack;
            }
            lua_pushboolean(L, isAttack ? 1 : 0);
            return 1;
        }},
                // GetPetActionCooldown(index) → start, duration, enable
                //
                // A flat zero, so a pet ability on cooldown was drawn ready and
                // clickable. The client has tracked pet spell cooldowns all
                // along - they arrive on the same packet as the player's.
                //
                // Only the remaining time is kept, not the original duration,
                // so the cooldown is reported as starting now and lasting what
                // is left. That draws exactly the remaining arc, which is the
                // part anyone reads, and it is what the item cooldown beside it
                // does for the same reason.
                {"GetPetActionCooldown", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            // Enable is one, not zero: zero switches the sweep off entirely,
            // and all three go straight into CooldownFrame_SetTimer.
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            // A command or a stance has no cooldown of its own.
            const float remaining = game::pet::isPetSpellAction(packed)
                ? gh->getSpellCooldown(game::pet::petActionId(packed)) : 0.0f;
            if (remaining <= 0.01f) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            lua_pushnumber(L, luaGetTimeNow());
            lua_pushnumber(L, remaining);
            lua_pushnumber(L, 1);
            return 3;
        }},
                {"PetAttack", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet() && gh->hasTarget())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kAttack),
                    gh->getTargetGuid());
            return 0;
        }},
                {"PetFollow", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kFollow), 0);
            return 0;
        }},
                {"PetWait", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kStay), 0);
            return 0;
        }},
                {"PetPassiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kPassive), 0);
            return 0;
        }},
                {"CastPetAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || !gh->hasPet() || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
            uint32_t packed = gh->getPetActionSlot(index - 1);
            uint32_t spellId = packed & 0x00FFFFFF;
            if (spellId != 0) {
                // CastPetAction(action, unit) - the unit is what a click-cast
                // binding on a unit frame passes, and dropping it sent the pet
                // at whatever was targeted instead of what was clicked. The
                // same fault CastSpellByID had, two files over.
                uint64_t target = 0;
                if (const char* uid = luaL_optstring(L, 2, nullptr)) {
                    std::string u(uid);
                    toLowerInPlace(u);
                    target = resolveUnitGuid(gh, u);
                }
                if (target == 0)
                    target = gh->hasTarget() ? gh->getTargetGuid() : gh->getPetGuid();
                gh->sendPetAction(packed, target);
            }
            return 0;
        }},
                {"TogglePetAutocast", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || !gh->hasPet() || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
            uint32_t packed = gh->getPetActionSlot(index - 1);
            uint32_t spellId = packed & 0x00FFFFFF;
            if (spellId != 0) gh->togglePetSpellAutocast(spellId);
            return 0;
        }},
                {"PetDismiss", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kAbandon), 0);
            return 0;
        }},
                // The other end of the ABANDON_PET dialog, which the pet's
                // unit menu opens. It was built, shown, and raised on accept.
                //
                // Not the same as PetDismiss above, which sends the pet away
                // and can be undone by calling it back. Abandon is permanent,
                // which is what the dialog in front of it is for.
                {"PetAbandon", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet()) gh->abandonPet();
            return 0;
        }},
                // CONFIRM_BUY_STABLE_SLOT's accept, from the stable panel.
                {"BuyStableSlot", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->buyStableSlot();
            return 0;
        }},
                // VOTE_BOOT_PLAYER's two buttons. The dialog is shown from
                // LFG_BOOT_PROPOSAL_UPDATE, which is fired, so a vote to kick
                // reached the player and neither answer went anywhere.
                //
                // Both buttons matter here: declining is a vote, not a
                // dismissal, and OnCancel sends it.
                {"SetLFGBootVote", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->lfgSetBootVote(lua_toboolean(L, 1) != 0);
            return 0;
        }},
                // CONFIRM_RESET_INSTANCES' accept, from /resetinstances and
                // the party menu.
                {"ResetInstances", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->resetInstances();
            return 0;
        }},
                // UNLEARN_SKILL's accept, from the skill panel's unlearn
                // button. The skill line comes from the button's own row.
                // Takes a skill id here, and FrameXML's one caller passes it
                // whatever it was handed as the popup's data - which in the
                // original client is the skills tab's row index, not an id.
                // Nothing shows UNLEARN_SKILL in this build (GetSkillLineInfo
                // answers isAbandonable false, so no row offers to unlearn), so
                // the two never meet; if the popup is ever raised, whichever
                // side passes the index has to resolve it through skillRows()
                // first, or this unlearns the skill whose id happens to equal a
                // row number.
                {"AbandonSkill", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto skillId = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (gh && skillId) gh->unlearnSkill(skillId);
            return 0;
        }},
                {"IsPetAttackActive", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->getPetCommand() == 2 ? 1 : 0); // 2=attack
            return 1;
        }},
                {"PetDefensiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kDefensive), 0);
            return 0;
        }},
                // The third stance, which was a no-op in another file entirely
                // - so a hunter could set a pet passive or defensive and not
                // aggressive, and the button for it did nothing.
                {"PetAggressiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kAggressive), 0);
            return 0;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }

    // The cursor, for this client's own windows to read. They keep a separate
    // held item of their own, so a drag out of a handed-over bag into a window
    // that is still this client's - the bank, the mailbox, a trade - had no way
    // to know anything had been picked up.
    //
    // Registered here rather than mirrored into the ui layer because this is
    // where the cursor is. Re-registered on every reload, which rebuilds the
    // Lua state, so the captured pointer is the current one; LuaEngine::shutdown
    // takes the bridge back down again.
    ui::frameXmlSetCursorBridge(
        [L](uint8_t& bag, uint8_t& slot) { return cursorWireSlot(L, bag, slot); },
        [L]() { clearCursorItem(L); });
}

} // namespace wowee::addons

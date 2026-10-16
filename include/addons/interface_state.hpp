#pragma once

// The interface's own state for one character in the world: which quest groups
// are folded, which dungeons are ticked, how the auction lists are sorted, what
// the cursor is carrying. None of it is the game's - the server is never told,
// or is told once as an argument - and all of it belongs to the character
// playing.
//
// One per core::CharacterSession, which outlives the Lua state reading it: a
// /reload builds a new state over the same InterfaceState - the interface is
// reloaded, the character is not; a logout destroys both, so the next
// character starts from these defaults. The bindings reach it through the Lua
// state that holds them (interfaceState in lua_api_helpers.hpp), never through
// a static - tools/function_static_check.py holds that at what it was left.

#include "game/calendar_data.hpp"
#include "game/craft_recipe.hpp"
#include "game/inventory_slots.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::addons {

/// What the cursor is carrying, if anything.
///
/// Shared because two sides need it: the bindings that pick an item up and put
/// it down, and the ones that describe a slot to the interface. A slot whose
/// item is on the cursor is "locked", which is what makes it draw greyed while
/// it is being dragged.
struct CursorItemSlot {
    int bag = -1;    ///< 0 for the backpack, 1-4 for a worn bag, -1 for none
    int slot = 0;    ///< 1-based within the bag, or the equipment slot
    bool equipped = false;
};

/// MERCHANT is a vendor's list index rather than anything the player owns, and
/// GUILDBANK a guild bank slot: both are bought or withdrawn when dropped.
enum class CursorType { NONE, SPELL, ITEM, MACRO, MERCHANT, MONEY, GUILDBANK };

/// One drawn row of the trade skill list: a recipe, or the heading of its
/// subclass.
struct TradeSkillRow {
    game::CraftRecipe recipe;
    std::string subclass;
    bool isHeader = false;
};

/// One key of an auction list's sort, as FrameXML sets it.
struct AuctionSortKey {
    std::string column;
    bool reverse = false;
};

struct InterfaceState {
    struct Cursor {
        CursorType type = CursorType::NONE;
        uint32_t id = 0;       ///< spellId, itemId, action slot or vendor index
        int slot = 0;          ///< source slot for placement
        /// Which container the item came out of, one of the four values
        /// game::slots documents: 0 the backpack, 1-4 a worn bag, -1 the
        /// paperdoll, kCursorNoSource the action bar.
        int bag = -1;
        uint64_t money = 0;    ///< copper, when the cursor carries money
        /// How much of a guild bank stack was taken, or zero for all of it.
        uint32_t split = 0;
        /// The slot the held item came from, as the slot bindings describe it.
        CursorItemSlot item;
        /// Whether the repair cursor is up at a vendor.
        bool repairMode = false;

        [[nodiscard]] uint32_t itemId() const { return type == CursorType::ITEM ? id : 0; }
        [[nodiscard]] uint64_t heldMoney() const { return type == CursorType::MONEY ? money : 0; }
        /// The paperdoll slot the held item came from, one-based, or zero.
        [[nodiscard]] int equipSlot() const {
            return (type == CursorType::ITEM && bag == -1) ? slot : 0;
        }
        [[nodiscard]] bool itemCameFromInventory() const {
            return type == CursorType::ITEM && game::slots::cursorSourceIsInventory(bag);
        }
    } cursor;

    struct QuestLog {
        /// Headers the player has collapsed, by group key (zoneOrSort).
        std::set<int32_t> collapsedGroups;
        /// The quest an abandon confirmation is about.
        uint32_t pendingAbandon = 0;
    } questLog;

    /// Which page of an open book is being read, zero-based.
    int itemTextPage = 0;
    /// Which stable slot the player has clicked.
    int selectedStableSlot = 0;

    struct TradeSkill {
        int selection = 0;
        std::string nameFilter;
        bool onlyMakeable = false;
        int subClassPick = 0;
        int invSlotPick = 0;
        /// The item level range typed into the search box; both zero is none.
        std::pair<int, int> levelRange{0, 0};
        std::set<std::string> collapsedSubClasses;
        /// Bumped whenever the drawn list is allowed to change shape; the rows
        /// are held still between bumps (see tradeSkillRows).
        uint64_t rowsGeneration = 1;
        uint64_t rowsBuiltAt = 0;
        std::vector<TradeSkillRow> rows;
    } tradeSkill;

    struct Trainer {
        int selection = 0;
        /// Seeded so the list, the dropdown's ticks and the setter agree on
        /// what each filter is; see trainerFilters.
        std::unordered_map<std::string, bool> filters{
            {"available", true},
            {"unavailable", false},
            {"used", false},
        };
        std::string search;
    } trainer;

    /// Talent points staged in the preview, by talent id.
    std::unordered_map<uint32_t, int> talentPreviewPoints;

    struct Mail {
        uint32_t money = 0;
        uint32_t cod = 0;
    } sendMail;

    /// Equipment slots marked to be left out of the next set save.
    std::set<int> equipmentSetIgnoredSlots;

    struct AuctionSellSlot {
        bool held = false;
        uint8_t bag = 0;
        uint8_t slot = 0;
    };
    struct Auction {
        /// Per list - "list", "owner", "bidder" - least significant key first.
        std::unordered_map<std::string, std::vector<AuctionSortKey>> sort;
        int selection = 0;
        AuctionSellSlot sellSlot;
    } auction;

    struct Lfg {
        std::unordered_map<int, bool> enabledDungeons;
        std::unordered_map<int, bool> collapsedHeaders;
    } lfg;

    struct Guild {
        bool showOffline = true;
        std::string sortField = "name";
        bool sortReverse = false;
        int selectedRosterRow = 0;
        /// The event log is asked for once per character, when its tab first
        /// opens with nothing in it.
        bool eventLogRequested = false;
    } guild;

    struct Who {
        std::string sortField = "name";
        bool sortReverse = false;
    } who;

    int selectedFriend = 0;
    int selectedIgnore = 0;
    int selectedFaction = 1;
    int selectedSkill = 0;
    int selectedDisplayChannel = 0;
    /// Whether the player opted out of loot rolls; the server keeps the real
    /// state and never sends it back.
    bool optedOutOfLoot = false;

    struct CalendarContextRow {
        int monthOffset = 0;
        int day = 0;
        int index = 0;
    };
    struct Calendar {
        /// (month, year); zero until first asked, then seeded from the clock.
        std::pair<int, int> viewedMonth{0, 0};
        game::CalendarEventDraft draft;
        CalendarContextRow contextRow;
    } calendar;

    /// Chat types whose player names are coloured by class.
    std::set<std::string> chatColorNameByClass;

    struct Battlefield {
        uint32_t selectedTypeId = 0;
        int selectedInstance = 0;
        /// The scoreboard's side: 1 Alliance, 0 Horde, -1 both.
        int scoreFaction = -1;
        std::string scoreSortColumn;
        bool scoreSortDescending = true;
    } battlefield;

    /// The hops of the route the flight map is drawing lines for.
    std::vector<uint32_t> taxiRouteShown;

    /// Which continent and zone the interface's world map is viewing.
    struct WorldMap {
        int continent = 0;
        int zone = 0;
    } worldMap;

    /// The failures the bindings have already logged - a key binding nothing
    /// runs, a macro the action bar holds and the account data does not - by
    /// what failed, so each is said once rather than on every press.
    std::set<std::string> failuresSaid;
    /// True the first time a failure is reported, false every time after.
    bool firstReport(const std::string& what) { return failuresSaid.insert(what).second; }
};

} // namespace wowee::addons

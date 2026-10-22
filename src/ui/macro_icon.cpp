#include "ui/macro_icon.hpp"
#include "core/logger.hpp"
#include "ui/macro_text.hpp"
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/game_state_adapter.hpp"
#include "ui/chat/input_modifier_adapter.hpp"
#include "game/account_data.hpp"
#include "game/game_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/spell_handler.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <SDL3/SDL_stdinc.h>

namespace wowee::ui {
namespace {
constexpr const char* question = "Interface\\Icons\\INV_Misc_QuestionMark";

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

uint32_t number(const std::string& text) {
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? value : 0;
}

std::string spellIcon(game::GameHandler& game, std::string name) {
    if (name.starts_with('!')) name = trim(name.substr(1));
    if (const auto id = number(name)) return game.getSpellIconPath(id);
    std::string rank;
    if (name.ends_with(')')) {
        const auto open = name.rfind('(');
        if (open != std::string::npos) {
            rank = lower(trim(name.substr(open + 1, name.size() - open - 2)));
            name = trim(name.substr(0, open));
        }
    }
    uint32_t best = 0;
    uint32_t bestRank = 0;
    const auto consider = [&](uint32_t id) {
        const auto* spell = game.spells().findSpell(id);
        if (!spell) return;
        const auto matches = [&](const std::string& candidate, const std::string& candidateRank) {
            return SDL_strcasecmp(candidate.c_str(), name.c_str()) == 0 &&
                (rank.empty() || SDL_strcasecmp(candidateRank.c_str(), rank.c_str()) == 0);
        };
        bool match = matches(spell->name, spell->rank);
        for (const auto& [localized, localizedRank] : spell->localizedNames)
            match = match || matches(localized, localizedRank);
        if (!match) return;
        const auto& label = spell->rank;
        const auto digit = label.find_first_of("0123456789");
        const auto level = digit == std::string::npos ? 0 : number(label.substr(digit));
        if (!best || level > bestRank || (level == bestRank && id > best)) {
            best = id;
            bestRank = level;
        }
    };
    for (auto id : game.spells().getKnownSpells()) consider(id);
    for (auto id : game.getPetSpells()) consider(id);
    return best ? game.getSpellIconPath(best) : std::string{};
}

std::string itemIcon(game::GameHandler& game, const std::string& name) {
    const auto& inventory = game.inventory().items();
    const auto slotIcon = [&](const game::ItemSlot& slot) {
        return slot.empty() ? std::string{} : game.getItemIconPath(slot.item.displayInfoId);
    };
    // /use 13 and /use 0 1 name inventory positions, not item IDs.
    std::istringstream positions(name);
    int bag = -1, slot = -1;
    std::string extra;
    if ((positions >> bag >> slot) && !(positions >> extra) && slot >= 1) {
        if (bag == 0 && slot <= inventory.getBackpackSize())
            return slotIcon(inventory.getBackpackSlot(slot - 1));
        if (bag >= 1 && bag <= game::Inventory::NUM_BAG_SLOTS && slot <= inventory.getBagSize(bag - 1))
            return slotIcon(inventory.getBagSlot(bag - 1, slot - 1));
        return {};
    }
    auto id = number(name);
    if (id >= 1 && id <= game::Inventory::NUM_EQUIP_SLOTS)
        return slotIcon(inventory.getEquipSlot(static_cast<game::EquipSlot>(id - 1)));
    if (name.starts_with("item:")) {
        const auto end = name.find(':', 5);
        id = number(name.substr(5, end == std::string::npos ? end : end - 5));
    }
    if (id) {
        const auto* info = game.inventory().getItemInfo(id);
        return info ? game.getItemIconPath(info->displayInfoId) : std::string{};
    }
    for (const auto& [itemId, info] : game.inventory().getItemInfoCache()) {
        if (SDL_strcasecmp(info.name.c_str(), name.c_str()) == 0)
            return game.getItemIconPath(info.displayInfoId);
    }
    return {};
}
}

std::string macroIconPath(const game::Macro& macro, game::GameHandler& game,
                          rendering::Renderer* renderer) {
    auto icon = macro.icon;
    std::replace(icon.begin(), icon.end(), '/', '\\');
    if (!icon.empty() && icon.find('\\') == std::string::npos) icon = "Interface\\Icons\\" + icon;
    auto normalized = lower(icon);
    if (normalized.ends_with(".blp")) normalized.resize(normalized.size() - 4);
    if (!icon.empty() && normalized != lower(question)) return icon;

    GameStateAdapter state(game, renderer);
    InputModifierAdapter modifiers;
    MacroEvaluator evaluator(state, modifiers);
    const auto select = [&](const std::string& text) {
        uint64_t target;
        return trim(evaluator.evaluate(text, target));
    };
    const auto resolve = [&](const std::string& text, bool itemFirst) {
        auto path = itemFirst ? itemIcon(game, text) : spellIcon(game, text);
        if (path.empty()) path = itemFirst ? spellIcon(game, text) : itemIcon(game, text);
        return path;
    };

    const auto missing = [&](const std::string& action) -> std::string {
        // Called on the interface thread, by both prefetch and Lua readers.
        // An unavailable action must be visible in the log, once per macro/action.
        static std::unordered_set<std::string> reported;
        if (!action.empty() && reported.insert(std::to_string(macro.id) + ":" + action).second)
            LOG_WARNING("Macro ", macro.id, " (", macro.name, "): cannot resolve icon for '",
                        action, "'; question-mark icon shown");
        return question;
    };
    const auto shown = macroShowtooltipArg(macro.body);
    if (!shown.empty() && shown != "__auto__") {
        const auto selected = select(shown);
        const auto path = selected.empty() ? std::string{} : resolve(selected, false);
        return path.empty() ? missing(selected) : path;
    }
    for (const auto& line : macroCommandLines(macro.body)) {
        const auto space = line.find_first_of(" \t");
        if (space == std::string::npos) continue;
        const auto command = lower(line.substr(0, space));
        if (command != "/cast" && command != "/use" && command != "/equip") continue;
        const auto selected = select(line.substr(space + 1));
        if (selected.empty()) continue;
        const auto path = resolve(selected, command != "/cast");
        // The first applicable action owns the automatic icon, even if its
        // spell/item is not available yet. Do not display a later action.
        return path.empty() ? missing(selected) : path;
    }
    return question;
}
}

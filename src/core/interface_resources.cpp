#include "core/application.hpp"
#include "ui/macro_icon.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "game/game_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/account_data_handler.hpp"
#include "core/logger.hpp"
#include <filesystem>

namespace wowee::core {
void Application::prepareInterfaceResources(float deltaTime) {
    const auto state = getState();
    if (addonCatalog_ && (state == AppState::CHARACTER_SELECTION ||
                         state == AppState::CHARACTER_CREATION ||
                         state == AppState::LOADING_WORLD || state == AppState::IN_GAME)) {
        widgetRenderer_.preloadInterface(
            std::filesystem::path(addonCatalog_->frameXmlDir()).parent_path().string());
    }
    // Icons belong to the chosen character. Account macros also wait until
    // login; arriving spell/item/account packets are picked up while loading,
    // and later updates continue warming unopened pages and bags in the world.
    const bool entering = gameHandler &&
        (gameHandler->getState() == game::WorldState::ENTERING_WORLD ||
         gameHandler->getState() == game::WorldState::IN_WORLD);
    if (entering) {
        const uint64_t character = gameHandler->getPlayerGuid();
        if (character != iconPrefetchCharacter_) {
            iconPrefetchCharacter_ = character;
            prefetchedSpellIcons_.clear();
            prefetchedItemIcons_.clear();
            iconPrefetchDelay_ = 0;
        }
        iconPrefetchDelay_ -= deltaTime;
        if (iconPrefetchDelay_ <= 0) {
            iconPrefetchDelay_ = 0.25f;
            const auto spell = [this](uint32_t id) {
                if (prefetchedSpellIcons_.contains(id)) return;
                const auto path = gameHandler->getSpellIconPath(id);
                prefetchedSpellIcons_.insert(id);
                if (path.empty()) {
                    LOG_WARNING("Interface prefetch: spell ", id, " has no icon in the active dataset; icon is unavailable");
                    return;
                }
                widgetRenderer_.prefetch(path, false, true);
            };
            // Prepare the book's actual projection as well as its images:
            // hidden spells and profession recipes are not book entries.
            for (const auto& tab : gameHandler->spells().getSpellBookTabs()) {
                widgetRenderer_.prefetch(tab.texture, false, true);
                for (uint32_t id : tab.spellIds) spell(id);
            }
            for (uint32_t id : gameHandler->getPetSpells()) spell(id);
            const auto item = [this](const game::ItemSlot& slot) {
                const uint32_t display = slot.item.displayInfoId;
                if (slot.empty() || !display || prefetchedItemIcons_.contains(display)) return;
                const auto path = gameHandler->getItemIconPath(display);
                prefetchedItemIcons_.insert(display);
                if (path.empty()) {
                    LOG_WARNING("Interface prefetch: item display ", display, " has no icon; icon is unavailable");
                    return;
                }
                widgetRenderer_.prefetch(path, false, true);
            };
            const auto& inventory = gameHandler->inventory().items();
            for (int i = 0; i < inventory.getBackpackSize(); ++i) item(inventory.getBackpackSlot(i));
            for (int i = 0; i < game::Inventory::NUM_EQUIP_SLOTS; ++i)
                item(inventory.getEquipSlot(static_cast<game::EquipSlot>(i)));
            for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b)
                for (int i = 0; i < inventory.getBagSize(b); ++i) item(inventory.getBagSlot(b, i));
            for (int i = 0; i < inventory.getKeyringSize(); ++i) item(inventory.getKeyringSlot(i));
            for (bool perCharacter : {false, true})
                for (const auto& macro : gameHandler->accountData().macros(perCharacter))
                    widgetRenderer_.prefetch(ui::macroIconPath(macro, *gameHandler, renderer.get()), false, true);
        }
        if (auto* addons = characterAddons(); addons && addons->getLuaEngine())
            widgetRenderer_.prefetchWidgets(addons->getLuaEngine()->widgets());
    } else {
        iconPrefetchCharacter_ = 0;
        iconPrefetchDelay_ = 0;
        prefetchedSpellIcons_.clear();
        prefetchedItemIcons_.clear();
    }
    widgetRenderer_.pumpResources();
}
}

#include "core/enchant_visual.hpp"

#include "core/logger.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"
#include "pipeline/m2_loader.hpp"

#include <mutex>
#include <string>
#include <unordered_set>

namespace wowee {
namespace core {

namespace {

// Once per key, from whichever thread asks: the spawner resolves enchants on
// its workers while the composer does on the main thread.
bool firstReport(const std::string& key) {
    static std::mutex mutex;
    static std::unordered_set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    return reported.insert(key).second;
}

} // namespace

EnchantVisualModels resolveEnchantVisualModels(const pipeline::GameTables& tables, uint32_t enchantId) {
    if (enchantId == 0) return {};

    constexpr std::string_view kNeededFor = "a weapon's enchant glow";
    const pipeline::RequiredTable sieDbc(tables, "SpellItemEnchantment.dbc", kNeededFor);
    // ItemVisuals: ID and five effect slots; ItemVisualEffects: ID and model.
    const auto visualsDbc = tables.table("ItemVisuals.dbc", 6, kNeededFor);
    const auto effectsDbc = tables.table("ItemVisualEffects.dbc", 2, kNeededFor);

    if (sieDbc->findRecordById(enchantId) < 0) {
        if (firstReport("enchant-id:" + std::to_string(enchantId))) {
            LOG_WARNING("Enchant visuals: enchant ", enchantId,
                        " is not in SpellItemEnchantment.dbc; its weapon is drawn without a glow");
        }
        return {};
    }

    return pipeline::resolveEnchantItemVisuals(enchantId, sieDbc.get(), visualsDbc.get(),
                                               effectsDbc.get(), &sieDbc.layout());
}

void attachEnchantVisuals(rendering::SceneCommands::Characters& characters, const pipeline::GameTables& tables,
                          uint32_t charInstanceId, uint32_t attachmentId, uint32_t enchantId,
                          const EnchantModelLoader& loadModel,
                          const std::function<uint32_t()>& allocateModelId) {
    characters.detachWeaponEffects(charInstanceId, attachmentId);
    if (enchantId == 0) return;

    const EnchantVisualModels models = resolveEnchantVisualModels(tables, enchantId);
    for (uint32_t visualSlot = 0; visualSlot < models.size(); ++visualSlot) {
        const std::string& path = models[visualSlot];
        if (path.empty()) continue;

        pipeline::M2Model model;
        if (!loadModel(path, model)) {
            if (firstReport("enchant-model:" + path)) {
                LOG_WARNING("Enchant visuals: ", path, " (enchant ", enchantId,
                            ", visual slot ", visualSlot,
                            ") did not load; the weapon is drawn without it");
            }
            continue;
        }

        if (characters.attachWeaponEffect(charInstanceId, attachmentId, visualSlot, model,
                                          allocateModelId())) {
            LOG_DEBUG("Enchant visual: ", path, " on attachment ", attachmentId,
                      " (enchant ", enchantId, ", visual slot ", visualSlot, ")");
        }
    }
}

} // namespace core
} // namespace wowee

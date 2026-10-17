#include "core/enchant_visual.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/character_renderer.hpp"

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

EnchantVisualModels resolveEnchantVisualModels(pipeline::AssetManager& assets, uint32_t enchantId) {
    if (enchantId == 0) return {};

    auto sieDbc     = assets.loadDBC("SpellItemEnchantment.dbc");
    auto visualsDbc = assets.loadDBC("ItemVisuals.dbc");
    auto effectsDbc = assets.loadDBC("ItemVisualEffects.dbc");
    if (!sieDbc || !sieDbc->isLoaded() || !visualsDbc || !visualsDbc->isLoaded() ||
        !effectsDbc || !effectsDbc->isLoaded()) {
        if (firstReport("enchant-tables")) {
            LOG_WARNING("Enchant visuals: SpellItemEnchantment, ItemVisuals or ItemVisualEffects.dbc "
                        "did not load; weapons are drawn without their enchant glows");
        }
        return {};
    }

    if (sieDbc->findRecordById(enchantId) < 0) {
        if (firstReport("enchant-id:" + std::to_string(enchantId))) {
            LOG_WARNING("Enchant visuals: enchant ", enchantId,
                        " is not in SpellItemEnchantment.dbc; its weapon is drawn without a glow");
        }
        return {};
    }

    const auto* sieL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
    return pipeline::resolveEnchantItemVisuals(enchantId, sieDbc.get(), visualsDbc.get(),
                                               effectsDbc.get(), sieL);
}

void attachEnchantVisuals(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                          uint32_t charInstanceId, uint32_t attachmentId, uint32_t enchantId,
                          const EnchantModelLoader& loadModel,
                          const std::function<uint32_t()>& allocateModelId) {
    renderer.detachWeaponEffects(charInstanceId, attachmentId);
    if (enchantId == 0) return;

    const EnchantVisualModels models = resolveEnchantVisualModels(assets, enchantId);
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

        if (renderer.attachWeaponEffect(charInstanceId, attachmentId, visualSlot, model,
                                        allocateModelId())) {
            LOG_DEBUG("Enchant visual: ", path, " on attachment ", attachmentId,
                      " (enchant ", enchantId, ", visual slot ", visualSlot, ")");
        }
    }
}

} // namespace core
} // namespace wowee

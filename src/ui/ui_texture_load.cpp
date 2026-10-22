#include "ui/ui_texture_cache.hpp"
#include "ui/ui_texture_load.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <unordered_set>

namespace wowee::ui {

namespace {
struct CacheRegistration {
    UiTextureCache* cache = nullptr;
    std::unordered_map<uint32_t, std::string> itemPaths;
};
std::unordered_map<pipeline::AssetManager*, CacheRegistration> caches;
}
void registerUiTextureCache(pipeline::AssetManager* assets, UiTextureCache* cache) {
    if (cache) caches[assets] = {cache, {}};
    else caches.erase(assets);
}
VkDescriptorSet uploadUiTextureFromBlp(pipeline::AssetManager* assetManager,
                                       const std::string& path,
                                       core::Window*, UiTextureLoad* why) {
    const auto it = caches.find(assetManager);
    if (it == caches.end()) {
        static std::unordered_set<std::string> reported;
        if (reported.insert(path).second)
            LOG_WARNING("Interface resource service is unavailable for '", path, "'; image is not drawn");
        if (why) *why = UiTextureLoad::NoContext;
        return VK_NULL_HANDLE;
    }
    auto& cache = *it->second.cache;
    cache.request(path, false, true);
    const auto* resident = cache.resident(path);
    if (why) *why = !resident ? UiTextureLoad::Pending
                             : *resident ? UiTextureLoad::Ok : UiTextureLoad::Failed;
    return resident ? *resident : VK_NULL_HANDLE;
}
VkDescriptorSet cachedIconTexture(uint32_t iconId, pipeline::AssetManager* assets,
    core::Window* window, const std::unordered_map<uint32_t, std::string>& paths,
    std::unordered_map<uint32_t, VkDescriptorSet>&) {
    const auto found = paths.find(iconId);
    return found == paths.end() ? VK_NULL_HANDLE : uploadUiTextureFromBlp(assets, found->second, window);
}

VkDescriptorSet itemIconTexture(uint32_t displayInfoId,
                                pipeline::AssetManager* assetManager,
                                core::Window* window) {
    if (displayInfoId == 0 || !assetManager) return VK_NULL_HANDLE;

    const auto registered = caches.find(assetManager);
    if (registered == caches.end()) return VK_NULL_HANDLE;
    auto& paths = registered->second.itemPaths;
    if (const auto it = paths.find(displayInfoId); it != paths.end())
        return it->second.empty() ? VK_NULL_HANDLE : uploadUiTextureFromBlp(assetManager, it->second, window);

    auto dbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!dbc) {
        core::Logger::getInstance().warning(
            "itemIconTexture: ItemDisplayInfo.dbc not loadable for displayInfoId=",
            displayInfoId);
        paths[displayInfoId] = {};
        return VK_NULL_HANDLE;
    }

    const int32_t recIdx = dbc->findRecordById(displayInfoId);
    if (recIdx < 0) {
        core::Logger::getInstance().warning(
            "itemIconTexture: displayInfoId=", displayInfoId,
            " not found in ItemDisplayInfo.dbc");
        paths[displayInfoId] = {};
        return VK_NULL_HANDLE;
    }

    const auto* layout = pipeline::getActiveDBCLayout()
                             ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo")
                             : nullptr;
    const std::string iconName =
        dbc->getString(static_cast<uint32_t>(recIdx), layout ? (*layout)["InventoryIcon"] : 5);
    if (iconName.empty()) {
        core::Logger::getInstance().warning(
            "itemIconTexture: displayInfoId=", displayInfoId, " recIdx=", recIdx,
            " has empty iconName field");
        paths[displayInfoId] = {};
        return VK_NULL_HANDLE;
    }

    const std::string iconPath = "Interface\\Icons\\" + iconName + ".blp";
    paths[displayInfoId] = iconPath;
    return uploadUiTextureFromBlp(assetManager, iconPath, window);
}

}  // namespace wowee::ui

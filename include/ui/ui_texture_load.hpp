#pragma once

/// Shared nonblocking interface resource requests. The WidgetRenderer owns
/// the cache for an asset dataset; legacy HUD/bag consumers use it too.

#include <cstdint>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

namespace wowee::core {
class Window;
}

namespace wowee::pipeline {
class AssetManager;
}

namespace wowee::ui {
class UiTextureCache;
// Producer-thread registration; removed before the owning cache is destroyed.
void registerUiTextureCache(pipeline::AssetManager* assets, UiTextureCache* cache);


/// Why an upload produced no texture. Ok is the only value that comes with a
/// usable descriptor set.
enum class UiTextureLoad {
    Ok,
    Pending,      ///< queued or waiting for its upload fence
    Failed,       ///< logged by the shared cache, not retried per frame
    NotFound,      ///< no such file, or it was empty
    DecodeFailed,  ///< the file is there and is not a readable BLP
    NoContext,     ///< no window or no Vulkan context yet
};

/// Queue a file and return its resident descriptor, or Pending. Never reads,
/// decodes, uploads or waits in the caller. Failures are logged once by cache.
VkDescriptorSet uploadUiTextureFromBlp(pipeline::AssetManager* assetManager,
                                       const std::string& path,
                                       core::Window* window,
                                       UiTextureLoad* why = nullptr);

/// Resolve an icon id, then use the same path cache as FrameXML.
VkDescriptorSet cachedIconTexture(
    uint32_t iconId, pipeline::AssetManager* assetManager, core::Window* window,
    const std::unordered_map<uint32_t, std::string>& paths,
    std::unordered_map<uint32_t, VkDescriptorSet>& cache);

/// One item icon, by its ItemDisplayInfo id.
///
/// The path comes from ItemDisplayInfo.dbc rather than from a table the caller
/// built, which is the one thing item icons do differently from the spell and
/// talent ones above. The cache is shared by everything that draws an item -
/// bags, the action bar, tooltips, dialogs - because they draw the same items
/// and there is no reason for each to upload its own copy.
VkDescriptorSet itemIconTexture(uint32_t displayInfoId,
                                pipeline::AssetManager* assetManager,
                                core::Window* window);

}  // namespace wowee::ui

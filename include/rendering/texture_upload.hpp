#pragma once

#include "rendering/normal_map_encoding.hpp"
#include "rendering/texture_cache.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace wowee {
namespace pipeline { struct BLPImage; }
namespace rendering {

class VkContext;
class VkTexture;

/// The sampler every texture in the cache carries: linear, repeating,
/// anisotropic, no bias. Whoever samples an image otherwise - clamped, with a
/// mip bias - binds its own sampler with the image's view, since another
/// renderer shares the same image.
bool giveCachedSampler(VkContext& ctx, VkTexture& texture);

/// A texture made and its copy recorded, not yet in the cache: what goes in
/// once the copy has run (TextureCache::insert). No texture when it could
/// not be made, which is said.
struct PreparedTexture {
    TextureKey key;
    std::shared_ptr<VkTexture> texture;
    uint64_t bytes = 0;
    TextureInfo info;
    /// Failed preparation can still have recorded copies into this image.
    /// Keep it alive until the batch fences, then discard it.
    bool publish = true;
};

/// Record a stored texture upload. Publication waits for the batch fence.
/// Under memory pressure, tries smaller existing mips before admission.
[[nodiscard]] PreparedTexture prepareStoredTexture(VkContext& ctx, const std::string& path,
                                                   const pipeline::BLPImage& image);

/// How this device keeps normal maps: BC7 where it can sample it, RGBA8
/// where it cannot.
[[nodiscard]] NormalMapFormat normalMapFormatFor(const VkContext& ctx);

/// Record the upload of a normal map encoded on a worker, without publishing.
[[nodiscard]] PreparedTexture prepareNormalMap(VkContext& ctx, const TextureKey& key, const EncodedNormalMap& map);

/// Record RGBA8 pixels and generated mips, without publishing.
[[nodiscard]] PreparedTexture preparePixels(VkContext& ctx, const TextureKey& key, const uint8_t* rgba,
                                             uint32_t width, uint32_t height, TextureInfo info);

/// The same, for a texture only its owner will find (TextureForm::Owned and
/// the like): never refused, and sampled however its owner says, since
/// nobody else binds it. Empty, said once per form, if the upload fails.
[[nodiscard]] TextureRef adoptPixels(VkContext& ctx, TextureForm form, const uint8_t* pixels,
                                     uint32_t width, uint32_t height, VkFormat format,
                                     bool generateMips, VkSamplerAddressMode addressMode,
                                     TextureInfo info = {});

} // namespace rendering
} // namespace wowee

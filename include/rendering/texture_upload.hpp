#pragma once

#include "rendering/texture_cache.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
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

/// Upload a file in the form the cache keeps files (TextureForm::Stored) and
/// hand it to the cache under `path`, normalized. Its transparency is read
/// once here, so every renderer sees the same answer whoever loaded it first.
///
/// When the whole image does not fit the budget, a smaller mip of it is
/// tried - a quarter of the memory each - before it is refused; the first
/// such texture of a stretch is said in the log. Empty, said once per path,
/// when it fails to upload or is refused: the caller draws its fallback.
[[nodiscard]] TextureRef cacheStoredTexture(VkContext& ctx, const std::string& path,
                                            const pipeline::BLPImage& image);

/// Upload RGBA8 pixels with generated mips and hand them to the cache under
/// `key`. Empty, said once per key, on failure or refusal.
[[nodiscard]] TextureRef cachePixels(VkContext& ctx, const TextureKey& key, const uint8_t* rgba,
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

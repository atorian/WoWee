#include "rendering/texture_upload.hpp"

#include "core/logger.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>

namespace wowee {
namespace rendering {

namespace {

/// Whether this is the first failure said for `what`: a failure is logged
/// once per path, not once per frame that asks for it again.
bool firstFailure(const std::string& what) {
    static std::mutex mutex;
    static std::unordered_set<std::string> said;
    std::lock_guard<std::mutex> lock(mutex);
    return said.insert(what).second;
}

/// The most mips a stored texture gives up before it is refused: a
/// sixteenth of its memory, a quarter of its resolution.
constexpr uint32_t kMaxSkippedLevels = 2;

} // namespace

bool giveCachedSampler(VkContext& ctx, VkTexture& texture) {
    return texture.createSampler(ctx.getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT);
}

TextureRef cacheStoredTexture(VkContext& ctx, const std::string& path, const pipeline::BLPImage& image) {
    TextureCache& cache = ctx.textures();
    const TextureKey key{normalizeTexturePath(path), TextureForm::Stored, 0};
    const TextureInfo info{.hasAlpha = image.hasTransparency(),
                           .alphaIsSilhouette = image.alphaIsSilhouette(),
                           .heightVariance = 0.0f};

    // Each mip given up is a quarter of what is left; the estimate decides
    // how many before anything is created on the GPU.
    uint32_t skip = 0;
    uint64_t estimate = image.approxUploadBytes();
    if (image.isBlockCompressed()) {
        while (!cache.reserve(estimate) && skip < kMaxSkippedLevels &&
               skip + 1 < image.mipmaps.size()) {
            ++skip;
            estimate /= 4;
        }
    }

    auto texture = std::make_shared<VkTexture>();
    if (!texture->uploadBLP(ctx, image, skip) || !giveCachedSampler(ctx, *texture)) {
        if (firstFailure("upload:" + key.path)) {
            LOG_WARNING("Texture cache: '", key.path, "' (", image.width, "x", image.height,
                        ") did not upload to the GPU; drawn with its fallback");
        }
        return {};
    }
    if (skip > 0 && firstFailure("skip:" + key.path)) {
        LOG_WARNING("Texture cache: no room for '", key.path, "' at ", image.width, "x", image.height,
                    "; kept at ", (image.width >> skip), "x", (image.height >> skip),
                    " while the budget is full");
    }
    const uint64_t bytes = texture->allocatedBytes();
    return cache.insert(key, std::move(texture), bytes, info);
}

TextureRef cachePixels(VkContext& ctx, const TextureKey& key, const uint8_t* rgba, uint32_t width,
                       uint32_t height, TextureInfo info) {
    auto texture = std::make_shared<VkTexture>();
    if (!texture->upload(ctx, rgba, width, height, VK_FORMAT_R8G8B8A8_UNORM, true) ||
        !giveCachedSampler(ctx, *texture)) {
        if (firstFailure("upload:" + key.path + "#" + textureFormName(key.form))) {
            LOG_WARNING("Texture cache: ", textureFormName(key.form), " '", key.path, "' (", width,
                        "x", height, ") did not upload to the GPU; drawn with its fallback");
        }
        return {};
    }
    const uint64_t bytes = texture->allocatedBytes();
    return ctx.textures().insert(key, std::move(texture), bytes, info);
}

TextureRef adoptPixels(VkContext& ctx, TextureForm form, const uint8_t* pixels, uint32_t width,
                       uint32_t height, VkFormat format, bool generateMips,
                       VkSamplerAddressMode addressMode, TextureInfo info) {
    auto texture = std::make_shared<VkTexture>();
    if (!texture->upload(ctx, pixels, width, height, format, generateMips) ||
        !texture->createSampler(ctx.getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR, addressMode)) {
        if (firstFailure(std::string("adopt:") + textureFormName(form))) {
            LOG_WARNING("Texture cache: a ", width, "x", height, " ", textureFormName(form),
                        " texture did not upload to the GPU; its owner draws its fallback");
        }
        return {};
    }
    const uint64_t bytes = texture->allocatedBytes();
    return ctx.textures().adopt(form, std::move(texture), bytes, info);
}

} // namespace rendering
} // namespace wowee

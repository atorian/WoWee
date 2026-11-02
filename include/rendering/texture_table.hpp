#pragma once

// Every texture the renderers sample, by number. One descriptor set, bound
// once a pass, holds the images in two arrays - plain and layered - and the
// samplers in a third; a texture is a TextureHandle naming its image's place
// and its sampler's (texture_table.glsli unpacks it). A draw that used to bind
// a set of its own textures carries numbers instead, so the draws of a pass
// can come from one indirect call.
//
// Images are not counted against sampler limits: Metal (MoltenVK) allows a
// few thousand samplers in a set but a million images, and the client's
// textures share a handful of samplers (VkContext::getOrCreateSampler), each
// placed here once and kept for the device's life.
//
// A place is given back by whoever took it, once no frame in flight can
// sample it: the texture cache from its retirement, an owner of an image of
// its own from its deferred release. Thread-safe.

#include "rendering/texture_cache.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

class TextureTable {
public:
    enum class Kind : uint8_t { Plain, Layered };

    /// The image's place in the low bits of a handle, the sampler's above.
    static constexpr uint32_t kImageBits = 20;
    static constexpr uint32_t kMaxImages = 1u << kImageBits;
    static constexpr uint32_t kMaxSamplers = 1u << (32 - kImageBits);

    struct Capacity {
        uint32_t plain = 0;
        uint32_t layered = 0;
        uint32_t samplers = 0;
    };
    /// What the table holds on a device with these limits: up to 65,536
    /// plain images, 4,096 layered ones and 1,024 samplers, less a reserve
    /// for the sets bound beside it.
    [[nodiscard]] static Capacity capacityFor(const VkPhysicalDeviceLimits& limits,
                                              const VkPhysicalDeviceVulkan12Properties& properties12);

    TextureTable() = default;
    ~TextureTable();
    TextureTable(const TextureTable&) = delete;
    TextureTable& operator=(const TextureTable&) = delete;

    bool initialize(VkDevice device, Capacity capacity);
    void shutdown();

    /// Set 'n' of every pipeline that samples by handle.
    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSet set() const { return set_; }

    /// A place for the view, sampled through `sampler`; kNoTextureHandle,
    /// said once a stretch, when the table is full.
    [[nodiscard]] TextureHandle add(VkImageView view, VkSampler sampler, Kind kind,
                                    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    /// The place back, once no frame in flight can sample it.
    void remove(TextureHandle handle, Kind kind);

    [[nodiscard]] uint32_t used(Kind kind) const;

private:
    struct Places {
        uint32_t capacity = 0;
        uint32_t next = 0;
        std::vector<uint32_t> free;
        uint32_t used = 0;
        bool fullSaid = false;
    };
    uint32_t samplerPlaceLocked(VkSampler sampler);
    Places& places(Kind kind) { return kind == Kind::Plain ? plain_ : layered_; }

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    mutable std::mutex mutex_;
    Places plain_, layered_;
    uint32_t samplerCapacity_ = 0;
    std::unordered_map<VkSampler, uint32_t> samplers_;
};

} // namespace rendering
} // namespace wowee

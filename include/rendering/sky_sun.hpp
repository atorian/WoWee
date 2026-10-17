#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/sky_celestial.hpp"
#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

/// The sun's disc and its glare, drawn as WoW.exe 3.3.5a draws them: two
/// textured sprites, Textures\sunCenter.blp and Textures\sunGlare.blp, each
/// tinted by the sun's colour from LightParams (LightIntBand channel 9).
///
/// The disc goes with the rest of the sky - alpha-blended, on the far plane,
/// cut and faded at the horizon (0x9ac660, 0x7edee0). The glare is drawn over
/// the finished world, added, with no depth test; whether the sun can be seen
/// is the caller's visibility (0x9ac400). Where and how large each is comes
/// from sky_celestial.hpp.
///
/// Pipeline layout: set 0 the per-frame UBO, set 1 the sprite's texture;
/// push = SpritePush.
class SkySun {
public:
    SkySun() = default;
    ~SkySun();
    SkySun(const SkySun&) = delete;
    SkySun& operator=(const SkySun&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    /// The two textures. Until they are loaded neither sprite is drawn, and
    /// the diagnostic says so.
    void loadTextures(pipeline::AssetManager* assets);
    void recreatePipelines();
    void shutdown();

    /// What the last renderDisc() did, for the sky diagnostic.
    struct DiscDraw {
        bool drawn = false;
        const char* skipped = "never rendered";  ///< why not, when not
        glm::vec3 direction{0.0f};
        float alpha = 0.0f;
    };

    /// The disc, in the sky pass. `colour` is LightParams' sunColor; `alpha`
    /// what weather leaves of it (sky_celestial::weatherFade).
    void renderDisc(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                    const sky_celestial::SunDisc& disc, const glm::vec3& colour, float alpha);
    [[nodiscard]] const DiscDraw& lastDiscDraw() const { return lastDiscDraw_; }

    /// The glare, over the finished world: toward `direction`, `size` units
    /// across at the sky distance, `colour` times `alpha`.
    void renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                     const glm::vec3& direction, float size, const glm::vec3& colour,
                     float alpha);

private:
    struct SpritePush {
        glm::mat4 model;
        glm::vec4 colour;   ///< rgb tint, a = alpha
        glm::vec4 horizon;  ///< x = centre height, y = scale, z = 1 to cut at the horizon
    };
    static_assert(sizeof(SpritePush) == 96, "SpritePush must match sky_sprite.*.glsl");

    VkPipeline buildPipeline(bool glare);
    void bindAndDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet perFrameSet,
                     VkDescriptorSet textureSet, const SpritePush& push);
    bool loadTexture(pipeline::AssetManager* assets, const char* path, VkTexture& texture,
                     VkDescriptorSet set, VkDescriptorSet& published);

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline discPipeline_ = VK_NULL_HANDLE;
    VkPipeline glarePipeline_ = VK_NULL_HANDLE;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAlloc_ = VK_NULL_HANDLE;

    VkTexture discTexture_;
    VkTexture glareTexture_;
    VkDescriptorSet discSetAllocated_ = VK_NULL_HANDLE;
    VkDescriptorSet glareSetAllocated_ = VK_NULL_HANDLE;
    /// Published once written; null until a texture has loaded.
    VkDescriptorSet discSet_ = VK_NULL_HANDLE;
    VkDescriptorSet glareSet_ = VK_NULL_HANDLE;

    DiscDraw lastDiscDraw_;
};

}  // namespace rendering
}  // namespace wowee

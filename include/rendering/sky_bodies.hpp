#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/frames_in_flight.hpp"
#include "rendering/sky_celestial.hpp"
#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

/// The sun and the moon, each a disc and a glare, drawn as WoW.exe 3.3.5a
/// draws them: textured sprites tinted by the sun's colour from LightParams
/// (LightIntBand channel 9) - the client gives the moon the same colour.
/// Textures\sunCenter.blp and Textures\moon.blp for the discs,
/// Textures\sunGlare.blp and Textures\moonGlare.blp for the glares.
///
/// The discs go with the rest of the sky - alpha-blended, on the far plane,
/// cut and faded at the horizon (0x9ac660, 0x7edee0). The glares are drawn
/// over the finished world, added, with no depth test; whether a body can be
/// seen is the caller's visibility (0x9ac400). Where and how large each is
/// comes from sky_celestial.hpp.
///
/// The client has a second moon, moon02, whose colour it never sets: it is
/// drawn with alpha 0 and shows only as a black band at the horizon and in a
/// storm. It is not drawn here.
///
/// Pipeline layout: set 0 the per-frame UBO, set 1 the sprite's texture;
/// push = SpritePush.
class SkyBodies {
public:
    enum class Body { Sun = 0, Moon = 1 };

    SkyBodies() = default;
    ~SkyBodies();
    SkyBodies(const SkyBodies&) = delete;
    SkyBodies& operator=(const SkyBodies&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    /// The four textures. A sprite whose texture has not loaded is not drawn,
    /// and the diagnostic says so.
    void loadTextures(pipeline::AssetManager* assets);
    void recreatePipelines();
    void shutdown();

    /// What the last renderDisc() of a body did, for the sky diagnostic.
    struct DiscDraw {
        bool drawn = false;
        const char* skipped = "never rendered";  ///< why not, when not
        glm::vec3 direction{0.0f};
        float alpha = 0.0f;
    };

    /// A disc, in the sky pass. `colour` is LightParams' sunColor; `alpha`
    /// what weather leaves of it (sky_celestial::weatherFade).
    void renderDisc(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Body body,
                    const sky_celestial::SkyDisc& disc, const glm::vec3& colour, float alpha);
    [[nodiscard]] const DiscDraw& lastDiscDraw(Body body) const {
        return lastDiscDraw_[static_cast<int>(body)];
    }

    /// A glare, over the finished world: toward `direction`, `size` units
    /// across at the sky distance, `colour` times `alpha`.
    void renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Body body,
                     const glm::vec3& direction, float size, const glm::vec3& colour,
                     float alpha);

    /// How much of each disc the world leaves in view, as WoW.exe measures it
    /// for its glare (0x9abe00): the disc's quad drawn on the far plane with
    /// no colour and its samples counted, over the samples its on-screen box
    /// holds. A frame slot late - the client's is a frame late. Every building,
    /// tree and hill that is drawn hides it, at any distance; the ray this
    /// replaced saw only the collision within twenty yards of the player, so a
    /// moon rising behind the inn shone through it.
    [[nodiscard]] float measuredVisibility(Body body) const {
        return measured_[static_cast<int>(body)];
    }
    [[nodiscard]] uint64_t visibilityFrame(Body body) const { return measuredFrames_[static_cast<int>(body)]; }
    /// Read this slot's answers from its last frame and reset its queries.
    /// Outside any render pass, after the slot's fence has been waited on.
    void beginFrameQueries(VkCommandBuffer cmd, uint32_t frame);
    /// Ask how much of a disc the world hides. Inside the scene pass, after
    /// the world has been drawn.
    void queryDiscVisibility(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Body body,
                             const sky_celestial::SkyDisc& disc, uint32_t frame,
                             const glm::mat4& view, const glm::mat4& projection,
                             VkExtent2D extent);

private:
    struct SpritePush {
        glm::mat4 model;
        glm::vec4 colour;   ///< rgb tint, a = alpha
        glm::vec4 horizon;  ///< x = centre height, y = scale, z = 1 to cut at the horizon
    };
    static_assert(sizeof(SpritePush) == 96, "SpritePush must match sky_sprite.*.glsl");

    /// One textured sprite: its file, texture and descriptor set.
    struct Sprite {
        const char* path = "";
        VkTexture texture;
        VkDescriptorSet allocated = VK_NULL_HANDLE;
        /// Published once written; null until the texture has loaded.
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    static constexpr int kSpriteCount = 4;
    Sprite& discSprite(Body body) { return sprites_[static_cast<int>(body) * 2]; }
    Sprite& glareSprite(Body body) { return sprites_[static_cast<int>(body) * 2 + 1]; }

    enum class SpritePass { Disc, Glare, Occlusion };
    VkPipeline buildPipeline(SpritePass pass);
    glm::mat4 discModel(const sky_celestial::SkyDisc& disc) const;
    void bindAndDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet perFrameSet,
                     VkDescriptorSet textureSet, const SpritePush& push);
    void loadSprite(pipeline::AssetManager* assets, Sprite& sprite);

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline discPipeline_ = VK_NULL_HANDLE;
    VkPipeline glarePipeline_ = VK_NULL_HANDLE;
    VkPipeline occlusionPipeline_ = VK_NULL_HANDLE;

    /// One occlusion query per body per frame slot: index frame * 2 + body.
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    bool queryIssued_[MAX_FRAMES_IN_FLIGHT][2] = {};
    uint64_t queryFrames_[MAX_FRAMES_IN_FLIGHT][2] = {};
    uint64_t measuredFrames_[2] = {};
    /// The samples the disc's on-screen box holds, when it was asked.
    float queryExpectedSamples_[MAX_FRAMES_IN_FLIGHT][2] = {};
    float measured_[2] = {};

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAlloc_ = VK_NULL_HANDLE;

    /// Sun disc, sun glare, moon disc, moon glare.
    Sprite sprites_[kSpriteCount];
    DiscDraw lastDiscDraw_[2];
};

}  // namespace rendering
}  // namespace wowee

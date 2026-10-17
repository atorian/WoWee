#pragma once

#include "rendering/liquid_scene.hpp"

#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

class Camera;
class VkContext;

/**
 * Water renderer (Vulkan) with planar reflections, Gerstner waves,
 * GGX specular, shoreline foam, and subsurface scattering.
 *
 * Draws the liquid a LiquidScene holds and nothing else: it is told of each
 * surface as it comes and goes (LiquidSceneListener) and keeps only the GPU
 * copy. What the water is and where - for swimming, the camera, footsteps -
 * is the scene's to answer.
 */
// Matches set 2 binding 3 in water.frag.glsl. Keep kMaxWakePoints in step with
// the MAX_WAKE_POINTS constant declared there.
constexpr int kMaxWakePoints = 32;

struct WaterFrameUBOData {
    glm::mat4 reflViewProj{1.0f};
    glm::vec4 wakeBounds{0.0f};              // xy = centre, z = cull radius, w = count
    glm::vec4 wakePoints[kMaxWakePoints]{};  // xy = pos, z = age 0..1, w = strength
};

class WaterRenderer : public LiquidSceneListener {
public:
    WaterRenderer();
    ~WaterRenderer() override;

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();

    // LiquidSceneListener: the GPU copy of each surface.
    void liquidSurfaceAdded(const LiquidSurface& surface) override;
    void liquidSurfaceRemoved(LiquidSurfaceId id) override;

    void recreatePipelines();

    // Separate 1x pass for MSAA mode - water rendered after MSAA resolve
    bool createWater1xPass(VkFormat colorFormat, VkFormat depthFormat);
    void createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                    VkImageView depthView, VkExtent2D extent);
    void destroyWater1xResources();
    bool hasWater1xPass() const { return water1xRenderPass != VK_NULL_HANDLE; }
    VkRenderPass getWater1xRenderPass() const { return water1xRenderPass; }
    VkFramebuffer getWater1xFramebuffer(uint32_t index) const {
        return index < water1xFramebuffers.size() ? water1xFramebuffers[index] : VK_NULL_HANDLE;
    }

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float time, bool use1x = false, uint32_t frameIndex = 0);
    void captureSceneHistory(VkCommandBuffer cmd,
                             VkImage srcColorImage,
                             VkImage srcDepthImage,
                             VkExtent2D srcExtent,
                             bool srcDepthIsMsaa,
                             uint32_t frameIndex = 0);

    // --- Planar reflection pass ---
    // Call sequence: beginReflectionPass → [render scene] → endReflectionPass
    bool beginReflectionPass(VkCommandBuffer cmd);
    void endReflectionPass(VkCommandBuffer cmd);

    // Compute reflected view matrix for a given water height
    static glm::mat4 computeReflectedView(const Camera& camera, float waterHeight);
    // Compute oblique clip projection to clip below-water geometry in reflection
    static glm::mat4 computeObliqueProjection(const glm::mat4& proj, const glm::mat4& view, float waterHeight);

    // Update the reflection UBO with reflected viewProj matrix
    void updateReflectionUBO(const glm::mat4& reflViewProj);

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    VkRenderPass getReflectionRenderPass() const { return reflectionRenderPass; }
    VkExtent2D getReflectionExtent() const { return {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}; }
    bool hasReflectionPass() const { return reflectionRenderPass != VK_NULL_HANDLE; }
    bool hasSurfaces() const { return !drawn_.empty(); }

    void setEnabled(bool enabled) { renderingEnabled = enabled; }
    bool isEnabled() const { return renderingEnabled; }

    void setRefractionEnabled(bool enabled);
    bool isRefractionEnabled() const { return refractionEnabled; }
    // Display brightness (1.0 = neutral). The scene-history capture used for
    // refraction bakes this in, so the shader divides it back out.
    // Size of the target the water is drawn into. Screen-space lookups derive
    // their UVs from this rather than from the refraction texture's own size,
    // which is deliberately smaller than the frame.
    void setRenderExtent(VkExtent2D e) { renderExtent_ = e; }

    /// Surfaces with geometry on the GPU.
    int getDrawnSurfaceCount() const { return static_cast<int>(drawn_.size()); }

private:
    /// The GPU copy of one LiquidSurface, with the little of it a draw needs.
    struct DrawnSurface {
        LiquidSurfaceId id = 0;
        glm::vec3 origin{0.0f};
        glm::vec3 stepX{0.0f};
        glm::vec3 stepY{0.0f};
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        uint16_t liquidType = 0;
        uint32_t wmoId = 0;
        uint8_t width = 0;
        uint8_t height = 0;

        ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        ::VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        int indexCount = 0;

        // Per-surface material UBO
        ::VkBuffer materialUBO = VK_NULL_HANDLE;
        VmaAllocation materialAlloc = VK_NULL_HANDLE;

        // Material descriptor set (set 1)
        VkDescriptorSet materialSet = VK_NULL_HANDLE;
    };

    void createWaterMesh(DrawnSurface& drawn, const LiquidSurface& surface);
    void destroyWaterMesh(DrawnSurface& drawn);
    void destroyAllDrawn();

    glm::vec4 getLiquidColor(uint16_t liquidType) const;
    float getLiquidAlpha(uint16_t liquidType) const;

    void updateMaterialUBO(DrawnSurface& drawn);
    VkDescriptorSet allocateMaterialSet();
    VkExtent2D refractionCaptureExtent() const;
    void createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat);
    void destroySceneHistoryResources();

    // Reflection pass resources
    void createReflectionResources();
    void destroyReflectionResources();

    VkContext* vkCtx = nullptr;

    // Pipeline
    VkPipeline waterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_WATER_SETS = 16384;

    VkSampler sceneColorSampler = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE;
    // Per-frame scene history to avoid race between frames in flight
    static constexpr uint32_t SCENE_HISTORY_FRAMES = 2;
    // One reflection/wake UBO slot per frame in flight, at an offset every
    // device accepts for a uniform buffer (Vulkan caps the alignment at 256).
    static constexpr VkDeviceSize kFrameUBOStride =
        (sizeof(WaterFrameUBOData) + 255) / 256 * 256;
    struct PerFrameSceneHistory {
        VkImage colorImage = VK_NULL_HANDLE;
        VmaAllocation colorAlloc = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthAlloc = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
    };
    PerFrameSceneHistory sceneHistory[SCENE_HISTORY_FRAMES];
    VkExtent2D sceneHistoryExtent = {.width = 0, .height = 0};
    bool sceneHistoryReady = false;
    mutable uint32_t renderDiagCounter_ = 0;

    // Planar reflection resources
    static constexpr uint32_t REFLECTION_WIDTH = 512;
    static constexpr uint32_t REFLECTION_HEIGHT = 512;
    VkRenderPass reflectionRenderPass = VK_NULL_HANDLE;
    VkFramebuffer reflectionFramebuffer = VK_NULL_HANDLE;
    VkImage reflectionColorImage = VK_NULL_HANDLE;
    VmaAllocation reflectionColorAlloc = VK_NULL_HANDLE;
    VkImageView reflectionColorView = VK_NULL_HANDLE;
    VkImage reflectionDepthImage = VK_NULL_HANDLE;
    VmaAllocation reflectionDepthAlloc = VK_NULL_HANDLE;
    VkImageView reflectionDepthView = VK_NULL_HANDLE;
    VkSampler reflectionSampler = VK_NULL_HANDLE;
    VkImageLayout reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Reflection UBO (mat4 reflViewProj)
    // Surface disturbance trail. Points age out, spreading as they go; the
    // per-point drift is what turns a swimmer's pair of emissions into a V.
    struct WakePoint {
        glm::vec2 pos{0.0f};
        glm::vec2 drift{0.0f};
        float age = 0.0f;        // seconds
        float life = 1.0f;       // seconds
        float strength = 0.0f;
    };
    std::vector<WakePoint> wakePoints_;
    WaterFrameUBOData frameUBO_{};
    glm::vec2 lastWakeEmitPos_{0.0f};
    bool hasWakeEmitPos_ = false;

    void uploadFrameUBO();

    ::VkBuffer reflectionUBO = VK_NULL_HANDLE;
    VmaAllocation reflectionUBOAlloc = VK_NULL_HANDLE;
    void* reflectionUBOMapped = nullptr;

    // Separate 1x water pass (used when MSAA is active)
    VkRenderPass water1xRenderPass = VK_NULL_HANDLE;
    VkPipeline water1xPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> water1xFramebuffers;

    std::vector<DrawnSurface> drawn_;
    bool renderingEnabled = true;
    bool refractionEnabled = false;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
};

} // namespace rendering
} // namespace wowee

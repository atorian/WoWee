#pragma once

#include "rendering/liquid_scene.hpp"
#include "rendering/ocean_waves.hpp"
#include "rendering/water_wake.hpp"

#include <array>
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
class SeaBedMap;
class VkContext;

/**
 * Water renderer (Vulkan) with planar reflections, the wind's sea
 * (OceanWaves), GGX specular, shoreline foam, and subsurface scattering.
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
    // The wind's sea (OceanWaves::cascades, seaState): per cascade xy = the
    // origin its patch is measured from, z = 1 / patch, w = patch, in yards;
    // and the sea as it stands - x = root mean square height, y = ride,
    // z = mean square slope.
    glm::vec4 oceanCascades[kOceanCascadeCount]{};
    glm::vec4 oceanSea{0.0f};
    // How much of each cascade's height and slope the water keeps at the
    // table's depths and fetches (OceanSea::keptTable).
    glm::vec4 oceanKept[OceanSea::kKeptEntries / 4]{};
    // The sea bed under the water (SeaBedMap): world to the map's clip
    // space, and x = the height of depth 0, y = yards to depth 1, z = 1 once
    // it has been drawn.
    glm::mat4 seaBedMatrix{1.0f};
    glm::vec4 seaBed{0.0f};
    // The wake round the camera (WaterWake::area): xy = the world position
    // of its corner, z = 1 / its side in yards, w = 1 once it has run.
    glm::vec4 wakeArea{0.0f};
    // Visible solar disc, independent of the legacy fixed diffuse light.
    glm::vec4 reflectionSun{0.0f, 0.0f, 1.0f, 0.0f}; // xyz direction, w visibility
    glm::vec4 reflectionSunColor{0.0f};
};

class WaterRenderer : public LiquidSceneListener {
public:
    WaterRenderer();
    ~WaterRenderer() override;

    /// `seaBed` is the map of what lies under the water, which the renderer
    /// draws and the water reads; it outlives this.
    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, const SeaBedMap* seaBed);
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

    struct SurfaceDraw {
        ::VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE;
        VkDescriptorSet materialSet = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        float waveAmp = 0, waveFreq = 0, waveSpeed = 0, basicType = 0, meshStep = 0, exposed = 0;
    };
    struct DrawList {
        std::vector<SurfaceDraw> surfaces;
        WaterFrameUBOData water{};
        OceanWaves::Frame ocean;
        WaterWake::Frame wake;
        bool inView = false;
    };
    DrawList extract(const Camera& camera);
    void recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                    const DrawList& list, bool use1x = false, uint32_t frameIndex = 0);
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

    // The mirrored camera's matrices: water_reflection.hpp.

    // Update the reflection UBO with reflected viewProj matrix
    void updateReflectionUBO(const glm::mat4& reflViewProj);

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    VkRenderPass getReflectionRenderPass() const { return reflectionRenderPass; }
    VkExtent2D getReflectionExtent() const { return reflectionExtent; }
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

    /// Move the wind's waves on by `deltaSeconds` under a wind of
    /// `windSpeed` yards a second (gust in) blowing toward `windDirection`,
    /// measured near `nearby` (the player). Call once per frame.
    void advanceWindWaves(double deltaSeconds, float windSpeed, glm::vec2 windDirection, glm::vec2 nearby);
    /// Move the wake round the camera on by `deltaSeconds`, centred near
    /// `nearby`, with `movers` the bodies in the water now. Once per frame.
    void advanceWake(double deltaSeconds, glm::vec2 nearby, const std::vector<WaterMover>& movers) {
        wake_.advance(deltaSeconds, nearby, movers);
    }
    /// A frame with none of the water in view: the wake waits (WaterWake::rest).
    void restWake(double deltaSeconds) { wake_.rest(deltaSeconds); }

    /// Whether any of the water is in `camera`'s view now, its crests in.
    /// Only then is there a sea to build (recordWaves), a wake to step, a
    /// sea bed to draw it over and a mirror to draw for it; render draws
    /// nothing otherwise. Once a frame, before any of them.
    bool updateInView(const Camera& camera);
    [[nodiscard]] bool inView() const { return inView_; }
    /// Build this frame's sea surface (OceanWaves::record) and step the
    /// wake (WaterWake::record): outside any render pass, before the water
    /// is drawn.
    void recordWaves(VkCommandBuffer cmd, const DrawList& list) {
        ocean_.record(cmd, list.ocean);
        wake_.record(cmd, list.wake);
    }

    /// Surfaces with geometry on the GPU.
    int getDrawnSurfaceCount() const { return static_cast<int>(drawn_.size()); }

    /// The highest water open to the wind within `halfExtent` yards of
    /// `centre` (a square), which the sea bed's map is drawn under; none
    /// when there is no such water there.
    [[nodiscard]] std::optional<float> highestOpenWaterNear(glm::vec2 centre, float halfExtent) const;

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
        /// 0 water, 1 ocean, 2 magma, 3 slime - LiquidType.dbc's word for it.
        uint8_t basicType = 0;
        LiquidShelter shelter = LiquidShelter::Open;

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

    /// The box `surface` may be drawn in, as far as its waves carry it, for
    /// culling it against the camera's view.
    struct Bounds {
        glm::vec3 min;
        glm::vec3 max;
    };
    [[nodiscard]] Bounds boundsOf(const DrawnSurface& surface) const;

    void createWaterMesh(DrawnSurface& drawn, const LiquidSurface& surface);
    void destroyWaterMesh(DrawnSurface& drawn);
    void destroyAllDrawn();

    /// The water's pipeline for `renderPass` at `samples`: tessellated near
    /// the camera where the device can, its own mesh otherwise.
    VkPipeline buildWaterPipeline(VkRenderPass renderPass, VkSampleCountFlagBits samples);
    /// The stages the push constants reach: the tessellation ones as well
    /// where the pipeline has them.
    VkShaderStageFlags pushStages_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    glm::vec4 getLiquidColor(uint8_t basicType) const;
    float getLiquidAlpha(uint8_t basicType) const;

    /// The wind's sea, as the surface every water surface samples.
    OceanWaves ocean_;
    /// What lies under the water; the renderer's.
    const SeaBedMap* seaBed_ = nullptr;
    /// What moves through the water leaves in it.
    WaterWake wake_;

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

    // Planar reflection resources
    VkExtent2D reflectionExtent{.width = 0, .height = 0};
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
    WaterFrameUBOData gameUBO_{};
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
    bool inView_ = false;
    // On from the start. The renderer keeps it always on, but did so only when
    // the interface applied its saved settings, so until then - and in a
    // renderer with no interface, scene_render - the water had no capture of
    // what is under it: no depth, no shoreline, no sea bed.
    bool refractionEnabled = true;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
};

} // namespace rendering
} // namespace wowee

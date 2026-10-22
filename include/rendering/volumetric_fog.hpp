#pragma once
#include "rendering/volumetric_medium.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>

namespace wowee {
namespace rendering {

class VkContext;

/**
 * Fog that light moves through: lit by the sun past the shadow map, so trees
 * and buildings cast shafts through it, by the zone's ambient light where
 * the sun does not reach, and by the torches, braziers and lava the renderer
 * already gathers for the surfaces around them.
 *
 * It is a volume over the camera's frustum - a grid of cells, 'froxels', each
 * a pixel tile deep by a slice of distance, the slices growing exponentially
 * so the near air is fine and the far air coarse. Two compute passes build it
 * every frame after the shadow map:
 *
 *   inject     fills each cell with how much its air scatters and absorbs
 *              and how much light arrives there, sampled at a jittered point
 *              and blended with last frame's cell, reprojected, so the jitter
 *              averages out instead of crawling;
 *   integrate  walks each column away from the camera and stores, at every
 *              depth, the light scattered toward the camera so far and how
 *              much of what lies behind still shows through.
 *
 * In legacy mode this covers the near weather air, in front of distance fog.
 * With the atmospheric setting enabled it spans View Distance and integrates
 * the zone-calibrated clear atmosphere and weather together. Surfaces and sky
 * consume this single transport result; geometry cutoff is composed separately.
 *
 * The volumes are double-buffered per frame in flight, as the shadow maps are:
 * a frame writes its own and reads the other one's cells as history.
 */
class VolumetricFog {
public:
    /// Off, then three volume sizes. The order is persisted in the settings
    /// file as an index, so add at the end.
    enum class Quality : int { Off = 0, Low = 1, Medium = 2, High = 3 };

    /// Legacy near-volume limits. Physical atmosphere uses a captured far
    /// distance per frame; logarithmic slices retain precision near the eye.
    static constexpr float kNear = 0.5f;
    static constexpr float kFar = 400.0f;

    /// What one frame's volume is built from.
    struct FrameInputs {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 cameraPos{0.0f};
        float farDistance = kFar;
        glm::vec4 atmosphere{0.0f}; ///< captured base, inverse height, thickness, mode
        glm::vec2 fogRange{0.0f};
        float time = 0.0f;           ///< seconds, for the drift of the mist
        VolumetricMedium medium;
        /// World height the densest air sits on.
        float layerBase = 0.0f;
        /// Yards over which the density falls by a factor of e above that.
        /// A few, so the mist lies on the ground - thick along a road, thin
        /// in the tree tops - and the air a few yards up is clearer than the
        /// air at the feet. Thirty spread it evenly through everything a
        /// player looks at, a haze with no ground to it.
        float layerHeight = 8.0f;
        /// Dust occupies a deeper layer than ground mist. Clear air is
        /// independent of both layers, including when viewed from above.
        float dustHeight = 80.0f;
        /// 0 is even mist; 1 lets it thin to nothing and pile to double.
        float noiseAmount = 0.6f;
        /// Brightness of lit air against a white surface in the same light, for
        /// every light that reaches it: the sun and the models' own lights
        /// alike, since it is the air's, not the light's. The surfaces take a
        /// light as its colour times N.L - LightParams' direct light, or a
        /// point light over the client's falloff: a Lambert reflector, sending
        /// 1/pi of what falls on it toward the eye. Air scattering evenly sends
        /// 1/(4 pi) each way - a quarter of that - and for the sun the phase
        /// function's forward lobe is what lifts the air toward it above that.
        ///
        /// 0.8 for the sun made the air over three times brighter than the
        /// light it is lit by, and the sky around the sun a white blot whatever
        /// the zone's sun was. 8 for the models' lights lit a campfire's air ten
        /// yards out as brightly as the ground a yard from it, and the fire's
        /// own flicker pulsed through the mist across the whole view.
        float lightScatter = 0.25f;
        /// The zone's glow around the sun (LightParams' sunHaloColor): the
        /// colour and brightness of the light the air scatters forward, toward
        /// an eye looking at the sun. See sunlit_air.glsli.
        glm::vec3 sunHaloColor{0.0f};
        /// Toward the sun's disc, and how much the sky round it shines (the
        /// glare's hours, cloud, sky model and weather; 0 at night). The
        /// forward glow is around the disc and goes with it, not with the
        /// light's direction, which never sets.
        glm::vec3 sunGlareDirection{0.0f, 0.0f, 1.0f};
        float sunGlareAmount = 0.0f;
    };

    VolumetricFog() = default;
    ~VolumetricFog();
    VolumetricFog(const VolumetricFog&) = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    /// The parts the per-frame descriptor layout needs before it exists: the
    /// sampler it bakes in and the neutral volume every set can bind.
    [[nodiscard]] bool initialize(VkContext* ctx);

    /// The compute passes. Set 0 is the renderer's per-frame layout, whose
    /// UBO they read; the shadow views are the per-frame-slot shadow maps.
    [[nodiscard]] bool createPipelines(VkDescriptorSetLayout perFrameLayout,
                                       const VkImageView shadowViews[2]);

    void shutdown();

    /// Takes effect at the next applyPendingQuality().
    void setQuality(Quality quality) { pendingQuality_ = quality; }

    /// Builds or frees the volumes for a quality change. Called between
    /// frames; waits for the device when there is something to change.
    /// Returns true when getVolumeView() now answers differently, so the
    /// per-frame descriptor sets must be written again.
    [[nodiscard]] bool applyPendingQuality();

    /// Volumes exist and a frame can build one.
    [[nodiscard]] bool isOn() const { return volumesReady_; }

    /// GPUPerFrameData::volumetricParams for a frame that builds its volume.
    [[nodiscard]] glm::vec4 frameParams(float farDistance = kFar) const;

    [[nodiscard]] VkSampler getSampler() const { return sampler_; }
    [[nodiscard]] VkImageView getNeutralView() const { return neutral_.view; }
    /// What set 0 binding 2 should hold for this frame slot: its integrated
    /// volume, or the neutral one while the fog is off.
    [[nodiscard]] VkImageView getVolumeView(uint32_t frame) const;

    /// Build this frame slot's volume. Outside any render pass, after the
    /// shadow map is drawn and left readable, and before anything samples
    /// binding 2.
    void record(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet perFrameSet,
                const FrameInputs& in);

    /// Forget the previous frames, after a teleport or a loading screen:
    /// reprojecting air from somewhere else would smear it across the view.
    void resetHistory() { historyValid_ = false; }

private:
    static constexpr uint32_t MAX_FRAMES = 2;

    struct Volume {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool createVolume(Volume& v, glm::uvec3 size, VkImageUsageFlags usage);
    void destroyVolume(Volume& v);
    bool createVolumes(glm::uvec3 size);
    void destroyVolumes();
    void writeComputeSets(uint32_t frame);

    VkContext* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;       // trilinear, clamped; owned by the context cache
    VkSampler depthSampler_ = VK_NULL_HANDLE;  // nearest, for reading the shadow map's depth
    Volume neutral_;                           // 1x1x1: no light scattered, everything through

    // Per frame slot: the lit cells and their running sum.
    Volume scatter_[MAX_FRAMES];
    Volume integrated_[MAX_FRAMES];
    glm::uvec3 size_{0};
    bool volumesReady_ = false;
    bool formatSupported_ = true;
    bool pipelinesReady_ = false;

    Quality pendingQuality_ = Quality::Off;
    Quality builtQuality_ = Quality::Off;

    VkImageView shadowViews_[MAX_FRAMES] = {};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline injectPipeline_ = VK_NULL_HANDLE;
    VkPipeline integratePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet computeSets_[MAX_FRAMES] = {};

    VkBuffer paramsUBO_[MAX_FRAMES] = {};
    VmaAllocation paramsAlloc_[MAX_FRAMES] = {};
    void* paramsMapped_[MAX_FRAMES] = {};

    // Reprojection state.
    glm::mat4 prevViewProj_{1.0f};
    glm::vec3 prevCameraPos_{0.0f};
    FrameInputs previousInputs_;
    bool historyValid_ = false;
    uint32_t frameCounter_ = 0;
    int lastRecordedFrame_ = -1;
};

} // namespace rendering
} // namespace wowee

#pragma once

#include "rendering/sea_bed_frame.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>

namespace wowee {
namespace rendering {

class VkContext;

/// The three views of what is under the water a drawing of the sea bed is
/// made from (sea_bed.glsli): the ground from straight above, the buildings
/// from above, and the buildings from below.
enum class SeaBedView : int { GroundFromAbove = 0, BuildingsFromAbove = 1, BuildingsFromBelow = 2 };

/**
 * What lies under the water near the camera: how deep the water is at each
 * point, as a map seen from straight above. The water reads it and its waves
 * lie down as the bed comes up (ocean_spectrum.glsli's oceanDepthFactor) -
 * on a beach, against a quay, round a lighthouse's footing.
 *
 * Only what is under the water is its bed: the views reach from the highest
 * water in the map down kRange yards, and a deck over the water or a bridge
 * high over a canal is above them - the water under it keeps its waves.
 *
 * A building is the bed only where it stands on the ground: a quay wall
 * comes down to the ground, a ship's hull floats over deep water, and the
 * water beside a ship has the sea's waves against it. Which is which is read
 * from the buildings seen from below as well as from above: a building whose
 * lowest surface comes down to the ground stands on it. A compute pass
 * (sea_bed_combine.comp) makes the one map the water reads out of the three.
 *
 * The views are drawn by the shadow pass's own casters - terrain and WMOs,
 * through their depth-only pipelines - with this drawing's matrices instead
 * of the sun's. Not the M2s: under the water a doodad is a boat's hull or a
 * pier's posts, too thin to still the water round them. It is drawn again
 * only when the camera has gone a quarter of the way to its edge, the
 * water's height has changed, or a few seconds have passed for the world
 * streamed in since.
 */
class SeaBedMap {
public:
    static constexpr int kTexels = 1024;
    static constexpr float kHalfExtent = 384.0f;  ///< yards: three quarters of a yard a texel
    static constexpr float kRange = 200.0f;       ///< yards below the water the views reach
    static constexpr double kRefreshSeconds = 3.0;

    SeaBedMap() = default;
    ~SeaBedMap();
    SeaBedMap(const SeaBedMap&) = delete;
    SeaBedMap& operator=(const SeaBedMap&) = delete;

    /// `shadowRenderPass` is the shadow pass's own, which its casters'
    /// pipelines were built for, where the device draws with render passes.
    [[nodiscard]] bool initialize(VkContext* ctx, VkRenderPass shadowRenderPass);
    void shutdown();

    /// Whether a camera at `nearby` over water as high as `waterHeight`
    /// wants the map drawn again at `now` seconds.
    [[nodiscard]] bool wantsDrawing(glm::vec2 nearby, float waterHeight, double now) const;

    /// Draw the map for a camera at `nearby` over water as high as
    /// `waterHeight`: begin it, draw each view's casters between beginView
    /// and endView with that view's matrix, and finish. Outside any render
    /// pass.
    void begin(glm::vec2 nearby, float waterHeight);
    [[nodiscard]] const SeaBedFrame& drawing() const { return drawing_; }
    /// World to the view's clip space.
    [[nodiscard]] glm::mat4 viewMatrix(SeaBedView view) const;
    void beginView(VkCommandBuffer cmd, SeaBedView view);
    void endView(VkCommandBuffer cmd, SeaBedView view);
    /// Make the map of the bed out of the views, and its fetch for a wind
    /// blowing toward `windToward` (sea_bed.glsli).
    void finish(VkCommandBuffer cmd, double now, glm::vec2 windToward);

    /// Whether the wind has turned far enough from the one the fetch was
    /// found for that it wants finding again over the same bed.
    [[nodiscard]] bool wantsFetch(glm::vec2 windToward) const;
    void refetch(VkCommandBuffer cmd, glm::vec2 windToward);

    /// The maps the water samples, both r32f in GENERAL over the same square,
    /// read texel by texel: the bed's depth as the views count it, and each
    /// point's effective fetch in yards (kOceanOpenFetch's share where a
    /// radial meets no shore within the map).
    /// What the depths mean is frame(); neither is drawn until finished once.
    [[nodiscard]] VkImageView view() const { return bed_.imageView; }
    [[nodiscard]] VkImageView fetchView() const { return effective_.imageView; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] const SeaBedFrame& frame() const { return frame_; }
    [[nodiscard]] bool isDrawn() const { return drawn_; }

private:
    bool createCombine();
    bool createFetch();
    void recordFetch(VkCommandBuffer cmd, glm::vec2 windToward);

    VkContext* ctx_ = nullptr;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;  // the shadow pass's; not ours
    std::array<AllocatedImage, 3> views_{};
    std::array<VkFramebuffer, 3> framebuffers_{};
    AllocatedImage bed_{};
    VkSampler sampler_ = VK_NULL_HANDLE;  // the context's cache owns it

    VkDescriptorSetLayout combineSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout combineLayout_ = VK_NULL_HANDLE;
    VkPipeline combinePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool combinePool_ = VK_NULL_HANDLE;
    VkDescriptorSet combineSet_ = VK_NULL_HANDLE;

    // The effective fetch (sea_bed.glsli), kFetchTexels a side over the
    // map's square: each radial's found back and forth between two turned
    // grids, ending in the first, and its share added into the third, which
    // lies along the map's axes and is what the water reads.
    static constexpr int kFetchTexels = 256;
    std::array<AllocatedImage, 2> fetch_{};
    AllocatedImage effective_{};
    VkDescriptorSetLayout fetchSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fetchLayout_ = VK_NULL_HANDLE;
    VkPipeline fetchPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool fetchPool_ = VK_NULL_HANDLE;
    /// [0] reads the second and writes the first, [1] the other way.
    std::array<VkDescriptorSet, 2> fetchSets_{};
    glm::vec2 fetchWind_{0.0f};

    SeaBedFrame frame_{};
    SeaBedFrame drawing_{};
    double drawnAt_ = -1e9;
    bool drawn_ = false;
};

} // namespace rendering
} // namespace wowee

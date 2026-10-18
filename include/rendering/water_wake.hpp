#pragma once

#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

class SeaBedMap;
class VkContext;

/// A body in the water, as the wake is told of it each frame.
struct WaterMover {
    uint32_t id = 0;            ///< the same body frame to frame
    glm::vec2 position{0.0f};   ///< world xy of its middle
    glm::vec2 facing{1.0f, 0.0f};  ///< unit: the way it faces, for when it is still
    float halfLength = 0.4f;    ///< yards, along the way it faces or moves
    float halfWidth = 0.25f;    ///< yards, across
    float head = 0.0f;          ///< the pressure head it puts on the water, yards (wakeHeadFor)
};

/// The pressure head a body puts on the water: as much of it as is under
/// the surface, to a yard, times what a yard of a body under the water
/// presses as (kWakeHeadPerYard), and nothing once it is wholly under by
/// more than its own reach of the surface - a diver deep down leaves the
/// surface alone. `bottom` and `top` are the body's lowest and highest
/// world heights, `surface` the water's.
float wakeHeadFor(float bottom, float top, float surface);

/**
 * The water's answer to what moves through it: a field of heights round the
 * camera, 128 yards a side, stepped sixty times a second by the iWave
 * equation with the surface's vertical derivative taken in its spectrum
 * (water_wake.glsli), so its waves run at water's own speeds and a body on
 * the move leaves Kelvin's wedge behind it. Each body in the water presses
 * on it with a patch the shape of what of it is under the surface, moved
 * smoothly between the frames it was seen in. The shore, from the sea bed's
 * map, holds the water still and throws waves back. The field moves with
 * the camera in whole texels, keeping what it holds where it was in the
 * world.
 *
 * The water reads `surface`: height, slopes and churn (water_wake_passes.glsli).
 */
class WaterWake {
public:
    static constexpr int kMaxMovers = 32;

    WaterWake() = default;
    ~WaterWake();
    WaterWake(const WaterWake&) = delete;
    WaterWake& operator=(const WaterWake&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx, const SeaBedMap* seaBed);
    void shutdown();

    /// The field `deltaSeconds` on, centred near `nearby` (the camera), with
    /// `movers` the bodies in the water now.
    void advance(double deltaSeconds, glm::vec2 nearby, const std::vector<WaterMover>& movers);
    /// A frame with none of the water in view, `deltaSeconds` long, in place
    /// of advance and record: the field is not stepped. When the water comes
    /// back it goes on from where it was; after long enough for its waves to
    /// have died away, it comes back still. Either way, what the bodies did
    /// unseen leaves no wake.
    void rest(double deltaSeconds);
    /// Its steps for this frame. Outside any render pass, before the water.
    void record(VkCommandBuffer cmd);

    [[nodiscard]] VkImageView surfaceView() const { return surface_.imageView; }
    [[nodiscard]] VkSampler surfaceSampler() const { return sampler_; }
    /// For the water's shaders: xy = the world position of the field's corner,
    /// z = 1 / its side in yards, w = 1 once it has run.
    [[nodiscard]] glm::vec4 area() const;

private:
    bool createPasses();

    VkContext* ctx_ = nullptr;
    const SeaBedMap* seaBed_ = nullptr;

    AllocatedImage state_{};
    AllocatedImage spectrum_{};
    AllocatedImage surface_{};
    VkSampler sampler_ = VK_NULL_HANDLE;  // the context's cache owns it

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline passPipeline_ = VK_NULL_HANDLE;
    VkPipeline fftPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kFrames = 2;
    std::array<VkDescriptorSet, kFrames> sets_{};
    std::array<VkBuffer, kFrames> moverBuffers_{};
    std::array<VmaAllocation, kFrames> moverAllocs_{};
    std::array<void*, kFrames> moverMapped_{};
    bool ready_ = false;

    // What the next record steps: the field's corner, how far it has moved
    // since the last record, how many steps, and the bodies' places.
    glm::ivec2 cornerTexel_{0};
    bool placed_ = false;
    glm::ivec2 pendingShift_{0};
    double pendingSeconds_ = 0.0;
    int pendingSteps_ = 0;
    bool ran_ = false;
    /// How long the water has been out of view, and whether the next record
    /// lays the field still first.
    double unseenSeconds_ = 0.0;
    bool stillPending_ = false;
    // As water_wake_passes.glsli's WakeMovers has them, in field yards.
    std::array<glm::vec4, kMaxMovers> place_{};
    std::array<glm::vec4, kMaxMovers> shape_{};
    std::array<glm::vec4, kMaxMovers> head_{};
    int moverCount_ = 0;
    int lastMoverCount_ = 0;
    std::unordered_map<uint32_t, glm::vec2> lastSeen_;
    bool saidTooMany_ = false;
};

} // namespace rendering
} // namespace wowee

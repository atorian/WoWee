#pragma once

#include "rendering/frames_in_flight.hpp"
#include "rendering/overhead_frame.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>

namespace wowee {
namespace rendering {

class VkContext;

/**
 * Where rain and snow can reach: the highest surface over each point near the
 * camera, as a depth map drawn from straight above - the ground, a roof, an
 * arch, a tree's crown. A drop below it is under cover and is not drawn, and
 * the splashes land on it, on the roofs as on the street.
 *
 * And what it says of the camera's own place, worked out on the GPU each frame
 * and read back as two numbers (shelter_probe.glsli): whether the sky is
 * over it (exposure) - the lens takes no rain under a porch and dries off
 * indoors - and how much of the sky its surroundings leave it (skyView),
 * which thins the rain in a street between high walls.
 *
 * Drawn by the shadow pass's own casters - terrain, buildings and doodads,
 * through their depth-only pipelines - with this map's matrix, as SeaBedMap
 * is. Drawn only while something falls, and again when the camera has gone a
 * quarter of the way to its edge, has climbed or dropped far, or a second and
 * a half has passed for the world streamed in since.
 */
class ShelterMap {
public:
    static constexpr int kTexels = 512;
    static constexpr float kHalfExtent = 64.0f;   ///< yards: a quarter of a yard a texel
    static constexpr float kAbove = 120.0f;       ///< yards over the camera the map's ceiling stands
    static constexpr float kRange = 480.0f;       ///< yards from the ceiling to depth 1
    static constexpr double kRefreshSeconds = 1.5;
    /// The soonest it is drawn again for the world having changed, while a
    /// city streams in a piece at a time.
    static constexpr double kWorldChangeSeconds = 0.25;

    ShelterMap() = default;
    ~ShelterMap();
    ShelterMap(const ShelterMap&) = delete;
    ShelterMap& operator=(const ShelterMap&) = delete;

    /// `shadowRenderPass` is the shadow pass's own, which its casters'
    /// pipelines were built for, where the device draws with render passes.
    [[nodiscard]] bool initialize(VkContext* ctx, VkRenderPass shadowRenderPass);
    void shutdown();

    /// Whether a camera at `camera` wants the map drawn again at `now`
    /// seconds. `world` says what is loaded - any number that changes when a
    /// building, a doodad or ground streams in or out: a map drawn before the
    /// house round the camera was there has no roof over it.
    [[nodiscard]] bool wantsDrawing(const glm::vec3& camera, double now, uint64_t world) const;
    /// Draw the map round `camera` with the world as `world` says it is:
    /// begin, draw the casters with matrix(), end. Outside any render pass.
    void begin(VkCommandBuffer cmd, const glm::vec3& camera, uint64_t world);
    [[nodiscard]] const OverheadFrame& drawing() const { return drawing_; }
    [[nodiscard]] glm::mat4 matrix() const { return drawing_.viewProjection(); }
    void end(VkCommandBuffer cmd, double now);

    /// Ask the map about the camera's place, into this frame slot's buffer.
    /// Outside any render pass, after the map is drawn.
    void probe(VkCommandBuffer cmd, uint32_t frameSlot, const glm::vec3& camera);
    /// Read what the probe of this slot found, once the slot's fence has
    /// passed: at the start of the frame that reuses it.
    void collect(uint32_t frameSlot);

    /// How open to the sky the camera was, 0 under cover to 1 in the open, as
    /// of the last probe read. 0 until a probe has been read: until the map
    /// says, the lens takes no rain.
    [[nodiscard]] float exposure() const { return exposure_; }
    /// How much of the sky the camera's surroundings leave it, 0-1 (the sky
    /// view factor); 1 until a probe has been read.
    [[nodiscard]] float skyView() const { return skyView_; }
    [[nodiscard]] uint64_t answerFrame() const { return answerFrame_; }

    /// The depth image the precipitation samples, in SHADER_READ_ONLY_OPTIMAL
    /// for the vertex and fragment stages; depth 1 (open everywhere) until
    /// first drawn. What a depth means is frame().
    [[nodiscard]] VkImageView view() const { return map_.imageView; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] const OverheadFrame& frame() const { return frame_; }
    [[nodiscard]] bool isDrawn() const { return drawn_; }
    [[nodiscard]] double drawnAt() const { return drawnAt_; }

private:
    VkContext* ctx_ = nullptr;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;  // the shadow pass's; not ours
    AllocatedImage map_{};
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;  // the context's cache owns it

    bool createProbe();

    struct Probe {
        AllocatedBuffer buffer{};  ///< vec4: exposure, sky view, written
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool pending = false;
        uint64_t frame = 0;
    };
    std::array<Probe, MAX_FRAMES_IN_FLIGHT> probes_{};
    VkDescriptorSetLayout probeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout probeLayout_ = VK_NULL_HANDLE;
    VkPipeline probePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool probePool_ = VK_NULL_HANDLE;

    OverheadFrame frame_{};
    OverheadFrame drawing_{};
    double drawnAt_ = -1e9;
    bool drawn_ = false;
    uint64_t drawingWorld_ = 0;
    uint64_t drawnWorld_ = 0;
    float exposure_ = 0.0f;
    float skyView_ = 1.0f;
    uint64_t answerFrame_ = 0;
};

}  // namespace rendering
}  // namespace wowee

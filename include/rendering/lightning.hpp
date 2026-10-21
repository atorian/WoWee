#pragma once

#include "rendering/frames_in_flight.hpp"
#include "rendering/lightning_storm.hpp"
#include "rendering/vk_utils.hpp"
#include <array>

namespace wowee::rendering {
class Camera;
class VkContext;

/// Depth-tested luminous ribbons, driven by the same strike as scene light and thunder.
class Lightning {
public:
    Lightning();
    ~Lightning();
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    void update(float dt, const Camera& camera, const LightningStorm::Ground& ground);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    void setEnabled(bool enabled) { storm_.setWeather(enabled, storm_.intensity()); }
    [[nodiscard]] bool isEnabled() const { return storm_.enabled(); }
    void setIntensity(float intensity) { storm_.setWeather(storm_.enabled(), intensity); }
    [[nodiscard]] float getIntensity() const { return storm_.intensity(); }
    void triggerStrike(const glm::vec3& cloud, const glm::vec3& ground) { storm_.strike(cloud, ground); }
    void reset() { storm_.reset(); }
    void reseed(uint32_t seed) { storm_.reseed(seed); }
    [[nodiscard]] const LightningStorm& storm() const { return storm_; }
    [[nodiscard]] std::vector<ThunderArrival> takeThunder() { return storm_.takeThunder(); }

private:
    struct Vertex { glm::vec3 position; float side; float brightness; };
    LightningStorm storm_;
    glm::vec3 eye_{0.0f};
    VkContext* ctx_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<AllocatedBuffer, MAX_FRAMES_IN_FLIGHT> vertices_{};
    // 28 main segments, at most 18 branches of five segments, six vertices each.
    static constexpr uint32_t kVertexCapacity = 1024;
};
} // namespace wowee::rendering

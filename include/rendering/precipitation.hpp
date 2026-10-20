#pragma once

#include "rendering/frames_in_flight.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/weather_mix.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <optional>

namespace wowee {
namespace rendering {

class LiquidScene;
class ShelterMap;
class VkContext;

/// What the precipitation is drawn from this frame.
struct PrecipitationFrame {
    WeatherMix mix;
    glm::vec3 camera{0.0f};
    /// The wind, yards a second along the ground, its gusts in it.
    glm::vec2 wind{0.0f};
    float dt = 0.0f;
    /// The water, for the splashes to ring on its surface rather than land on
    /// the bed under it. Empty in a scene with none.
    const LiquidScene* water = nullptr;
    double now = 0.0;  ///< seconds, for how often the water is looked at again
    /// How much of the sky the camera's surroundings leave it, 0-1
    /// (ShelterMap::skyView): in a street between high walls less of the
    /// rain is seen, the far drops least of all, and the walls break the wind.
    float openSky = 1.0f;
    VkExtent2D viewport{.width = 1, .height = 1};
};

/**
 * Rain, snow and sand falling round the camera, as The Witcher 3 draws them:
 * thousands of lit streaks and flakes in two boxes - one close and dense, one
 * wide - that are fixed in the world and wrapped round the camera, so walking
 * moves through them; splashes on whatever the rain lands on, roofs too; sand
 * as grains driven along the wind and drifting veils of dust over the ground.
 * None of it falls under cover (ShelterMap).
 *
 * Nothing per drop is kept or uploaded: precipitation.vert.glsl places each
 * from its index. What the CPU keeps is how far each layer has fallen and
 * blown, accumulated from the wind as it changes.
 */
class Precipitation {
public:
    Precipitation() = default;
    ~Precipitation();
    Precipitation(const Precipitation&) = delete;
    Precipitation& operator=(const Precipitation&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, const ShelterMap& shelter);
    void shutdown();

    /// The pass it is recorded into: the scene's, or the water's after the
    /// water when that has its own - the drops go over the water, not under
    /// it. Call before initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }
    void recreatePipelines();

    void update(const PrecipitationFrame& frame);
    /// `frameSlot` is the frame in flight, whose copy of the water grid it
    /// reads.
    struct Frame;
    Frame extract() const;
    void record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, uint32_t frameSlot, const Frame& input);

    /// The Weather Detail setting: how much of the weather to draw, a quality
    /// knob and not a forecast - it thins every storm without making it a
    /// different one.
    void setDensityScale(float scale) { densityScale_ = std::clamp(scale, 0.0f, 1.0f); }
    [[nodiscard]] float densityScale() const { return densityScale_; }
    void setSplashesEnabled(bool on) { splashes_ = on; }
    [[nodiscard]] bool splashesEnabled() const { return splashes_; }

    /// How many drops, flakes, grains and splashes the last update drew.
    [[nodiscard]] uint32_t drawnCount() const { return drawnCount_; }

private:
    enum class LayerKind : int { Rain = 0, Snow = 1, Grains = 2, Sheets = 3, Splash = 4 };
    struct Layer {
        LayerKind kind = LayerKind::Rain;
        uint32_t fullCount = 0;    ///< drops at full weather and full detail
        float boxWidth = 0.0f;     ///< yards; for splashes, the disc's radius
        float boxHeight = 0.0f;
        float below = 0.35f;       ///< how much of the box lies under the camera
        float size = 0.0f;         ///< yards
        float fallSpeed = 0.0f;    ///< yards a second, down
        float windFollow = 1.0f;   ///< how much of the wind it is carried at
        float streakSeconds = 0.0f;
        float opacity = 1.0f;
        uint32_t seed = 0;
        /// How much of the layer is left with the sky closed in all round.
        float enclosed = 1.0f;
        // This frame's.
        glm::dvec3 drift{0.0};
        glm::vec3 velocity{0.0f};
        uint32_t count = 0;
        float alpha = 0.0f;
    };
    static constexpr size_t kLayers = 7;

    bool buildPipeline();
    void lookAtWater(const PrecipitationFrame& frame);

    /// The water's height over a square round the camera, a cell at a time,
    /// for the splashes (precipitation.vert.glsl's WaterGrid).
    static constexpr int kWaterCells = 24;
    static constexpr float kWaterReach = 18.0f;  ///< yards from the middle to an edge
    struct WaterGrid {
        glm::vec4 origin{0.0f};  ///< xy the square's low corner, z a cell's size, w cells a side
        float heights[kWaterCells * kWaterCells]{};
    };

    VkContext* ctx_ = nullptr;
    const ShelterMap* shelter_ = nullptr;
    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets_{};
    std::array<AllocatedBuffer, MAX_FRAMES_IN_FLIGHT> waterBuffers_{};
    WaterGrid water_{};
    glm::vec2 waterCentre_{1e9f};
    double waterLookedAt_ = -1e9;

    std::array<Layer, kLayers> layers_{};
    PrecipitationFrame frame_{};
    glm::vec3 tint_{1.0f};
    double time_ = 0.0;
    /// openSky, eased so walking out of an alley does not switch the rain.
    float openSky_ = 1.0f;
    bool openSkyKnown_ = false;
    float densityScale_ = 1.0f;
    bool splashes_ = true;
    uint32_t drawnCount_ = 0;
    bool firstDrawSaid_ = false;
public:
    struct Frame {
        std::array<Layer, kLayers> layers;
        WaterGrid water;
        glm::vec3 camera{0}, tint{1};
        VkExtent2D viewport{};
        double time = 0;
        uint32_t drawn = 0;
    };

};

}  // namespace rendering
}  // namespace wowee

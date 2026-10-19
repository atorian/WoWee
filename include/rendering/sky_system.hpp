#pragma once

#include <memory>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "rendering/sky_celestial.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;
class Skybox;
class StarField;
class Clouds;
class LensFlare;
class SkyBodies;
class LightingManager;

/**
 * Sky rendering parameters (extracted from LightingManager)
 */
struct SkyParams {
    // Sun/moon positioning
    glm::vec3 directionalDir{0.0f, -1.0f, 0.3f};
    /// The sun's and the moon's disc and glare colour: LightingParams'
    /// sunColor, not the light the sun casts.
    glm::vec3 sunColor{1.0f, 0.97f, 0.87f};
    /// Where each disc stands at this hour - its own path, not the light's.
    sky_celestial::SkyDisc sunDisc;
    sky_celestial::SkyDisc moonDisc;
    /// The clouds' sunlit, fill and base colours (LightParams' cloud* fields).
    glm::vec3 cloudSunlitColor{1.0f, 0.78f, 0.54f};
    glm::vec3 cloudFillColor{0.55f, 0.55f, 0.6f};
    glm::vec3 cloudBaseColor{0.35f, 0.35f, 0.4f};

    // Sky colors (for skybox tinting/blending)
    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};
    glm::vec3 skySmogColor{0.5f, 0.6f, 0.7f};
    glm::vec3 skyFogColor{0.5f, 0.6f, 0.7f};

    // Atmospheric effects
    float cloudDensity = 0.0f;      // 0-1
    float fogDensity = 0.0f;        // 0-1
    float horizonGlow = 0.3f;       // 0-1
    float weatherIntensity = 0.0f;  // 0-1 (rain/snow intensity, attenuates lens flare)
    // How much of the sun's glare the camera sees, eased, 0 to 1: its hours,
    // the cloud and weather in front of it, a sky model over it and whatever
    // stands between it and the eye (Renderer::updateGlares). The lens
    // flare is drawn by it. Zero for a pass that is not the camera's own.
    float sunGlareVisibility = 0.0f;
    // Whether the sun's and moon's discs are drawn. Not in the sky captured for
    // the distant haze: a far building hazes toward the captured sky at its
    // own pixel, and with the discs in the capture a moon rising behind a
    // misty inn showed through the inn's walls.
    bool drawBodies = true;

    // Time
    float timeOfDay = 12.0f;    // 0-24 hours
    float gameTime = -1.0f;     // Server game time, hours since midnight (-1 = use fallback)

    // Skybox selection (future: from LightSkybox.dbc)
    uint32_t skyboxModelId = 0;
    bool skyboxHasStars = false;  // Does loaded skybox include baked stars?
    bool useOriginalSkybox = false; // Original camera-centered client M2 is active
};

/**
 * Unified sky rendering system
 *
 * Coordinates skybox (authoritative), the sun and the moon, and fallback
 * procedural stars. Driven by lighting system data.
 *
 * Architecture:
 * - Skybox is PRIMARY (includes baked stars from M2 models)
 * - SkyBodies draws the sun's and the moon's discs, as the client does
 * - StarField is DEBUG/FALLBACK only (disabled when skybox has stars)
 */
class SkySystem {
public:
    SkySystem();
    ~SkySystem();

    /**
     * Initialize sky system components.
     * @param ctx            Vulkan context (required for Vulkan renderers)
     * @param perFrameLayout Descriptor set layout for set 0 (camera UBO)
     */
    bool initialize(VkContext* ctx = nullptr, VkDescriptorSetLayout perFrameLayout = VK_NULL_HANDLE);
    void shutdown();

    /**
     * Update sky system (time, moon phases, etc.)
     */
    void update(float deltaTime);

    /**
     * Render complete sky.
     * @param cmd         Active Vulkan command buffer
     * @param perFrameSet Per-frame descriptor set (set 0, camera UBO)
     * @param camera      Camera for legacy sub-renderers (lens flare, etc.)
     * @param params      Sky parameters from lighting system
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                const Camera& camera, const SkyParams& params);

    /**
     * Enable/disable procedural stars (DEBUG/FALLBACK)
     * Default: OFF (stars come from skybox)
     */
    void setProceduralStarsEnabled(bool enabled) { proceduralStarsEnabled_ = enabled; }
    [[nodiscard]] bool isProceduralStarsEnabled() const { return proceduralStarsEnabled_; }

    /**
     * Enable/disable debug sky mode (forces procedural stars even with skybox)
     */
    void setDebugSkyMode(bool enabled) { debugSkyMode_ = enabled; }
    [[nodiscard]] bool isDebugSkyMode() const { return debugSkyMode_; }

    // Component accessors (for direct control if needed)
    [[nodiscard]] Skybox*    getSkybox()    const { return skybox_.get(); }
    [[nodiscard]] StarField* getStarField() const { return starField_.get(); }
    [[nodiscard]] Clouds*    getClouds()    const { return clouds_.get(); }
    [[nodiscard]] LensFlare* getLensFlare() const { return lensFlare_.get(); }
    [[nodiscard]] SkyBodies* getBodies()    const { return bodies_.get(); }

    /// The sun's and the moon's textures, once the client's data is open.
    void loadTextures(pipeline::AssetManager* assets);

    /// Whether the last render() drew the sun, and if not why - including
    /// when a sky model standing in for the client's sky left it out.
    [[nodiscard]] bool celestialSkipped() const { return celestialSkipped_; }

private:
    std::unique_ptr<Skybox>    skybox_;      // Authoritative sky
    std::unique_ptr<StarField> starField_;   // Fallback procedural stars
    std::unique_ptr<Clouds>    clouds_;      // Cloud layer
    std::unique_ptr<LensFlare> lensFlare_;   // Sun lens flare
    std::unique_ptr<SkyBodies> bodies_;      // Sun and moon, discs and glares

    bool proceduralStarsEnabled_ = false;
    bool debugSkyMode_ = false;
    bool celestialSkipped_ = false;
    bool initialized_ = false;
};

} // namespace rendering
} // namespace wowee

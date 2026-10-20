#pragma once
#include "rendering/rt_distance.hpp"

#include "rendering/overlay_system.hpp"

#include "rendering/camera.hpp"
#include "rendering/frame_settings.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/lens_drops.hpp"
#include "rendering/local_lights.hpp"
#include "rendering/viewer_motion.hpp"
#include "rendering/weather_mix.hpp"
#include "rendering/world_environment.hpp"

#include <optional>
#include <memory>
#include <vector>

namespace wowee::rendering {

/// Values extracted at the end of Present. Owns its camera, sampled light,
/// weather and local lights; no references into the next game's frame.
/// Geometry draw lists and interface draw data are extracted in later stages.
struct FrameScene;

struct FrameInputs {
    std::shared_ptr<const FrameScene> scene;
    Camera camera;
    glm::mat4 shadowMatrix{0.0f};
    glm::vec3 shadowCenter{0.0f};
    OverlaySystem::Selection selection;
    glm::vec3 characterPosition{0.0f};
    glm::vec4 playerPos{0.0f};
    glm::vec4 playerWake{0.0f};
    float characterYaw = 0.0f;
    uint32_t characterInstance = 0;
    WorldEnvironment environment;
    ViewerMotion viewer;
    LightingParams lighting;
    LightingParams weatherLight;
    bool hasLighting = false;
    float timeOfDay = 12.0f;
    float time = 0.0f;
    float deltaTime = 0.0f;
    WeatherMix weather;
    WindGpu wind;
    glm::vec2 windDirection{0.0f};
    LensDrops lensDrops;
    glm::vec2 lensWind{0.0f};
    float sunGlare = 0.0f;
    float sunGlareSky = 0.0f;
    float moonGlare = 0.0f;
    float viewDistance = 1200.0f;
    float shadowDistance = 300.0f;
    bool shadows = true;
    bool worldInView = false;
    bool terrainEnabled = true;
    bool drawSkyModels = false;
    bool hideProceduralSky = false;
    bool sunShafts = true;
    bool sharpStars = true;
    bool minimapEnabled = false;
    uint32_t windowWidth = 0, windowHeight = 0;
    int rtMode = 0;
    float rtDistance = kDefaultRtDistance;
    int fogQuality = 0;
    uint32_t msaaSamples = 1;
    FramePostProcessSettings postProcess;
    float fogDensity = 1.0f;
    float fogTargetExtinction = 0.0f;
    float fogGround = 0.0f;
    std::optional<float> cameraWaterDepth;
    std::optional<float> waterHeight;
    std::optional<float> reflectionWaterHeight;
    std::optional<float> seaWaterHeight;
    bool waterIsWmo = false;
    bool canal = false;
    uint64_t shelterRevision = 0;
    std::vector<LocalLight> localLights;
};

} // namespace wowee::rendering

#include "rendering/sky_system.hpp"
#include "rendering/sun_direction.hpp"
#include "rendering/skybox.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/sky_bodies.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

SkySystem::SkySystem() = default;

SkySystem::~SkySystem() {
    shutdown();
}

bool SkySystem::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    if (initialized_) {
        LOG_WARNING("SkySystem already initialized");
        return true;
    }

    LOG_INFO("Initializing sky system");

    // Skybox (Vulkan)
    skybox_ = std::make_unique<Skybox>();
    if (!skybox_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize skybox");
        return false;
    }

    // The sun and the moon, discs and glares, as the client draws them.
    bodies_ = std::make_unique<SkyBodies>();
    if (!bodies_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize the sun and moon");
        return false;
    }

    // Procedural stars - fallback / debug (Vulkan)
    starField_ = std::make_unique<StarField>();
    if (!starField_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize star field");
        return false;
    }
    starField_->setEnabled(false); // Off by default; skybox is authoritative

    // Clouds (Vulkan)
    clouds_ = std::make_unique<Clouds>();
    if (!clouds_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize clouds");
        return false;
    }

    // Lens flare (Vulkan)
    lensFlare_ = std::make_unique<LensFlare>();
    if (!lensFlare_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize lens flare");
        return false;
    }

    initialized_ = true;
    LOG_INFO("Sky system initialized successfully");
    return true;
}

void SkySystem::shutdown() {
    if (!initialized_) {
        return;
    }

    LOG_INFO("Shutting down sky system");

    if (lensFlare_)  lensFlare_->shutdown();
    if (clouds_)     clouds_->shutdown();
    if (starField_)  starField_->shutdown();
    if (bodies_)     bodies_->shutdown();
    if (skybox_)     skybox_->shutdown();

    lensFlare_.reset();
    clouds_.reset();
    starField_.reset();
    bodies_.reset();
    skybox_.reset();

    initialized_ = false;
}

void SkySystem::update(float deltaTime) {
    if (!initialized_) {
        return;
    }

    if (skybox_)    skybox_->update(deltaTime);
    if (starField_) starField_->update(deltaTime);
    if (clouds_)    clouds_->update(deltaTime);
}

SkySystem::Frame SkySystem::extract() const {
    Frame frame;
    frame.sky = skybox_ && skybox_->isEnabled();
    frame.clouds = clouds_ && clouds_->isEnabled();
    frame.proceduralStars = proceduralStarsEnabled_;
    frame.debugSky = debugSkyMode_;
    frame.flare = lensFlare_ && lensFlare_->isEnabled();
    if (clouds_) frame.cloudOffset = clouds_->getWindOffset();
    if (starField_) frame.twinkleTime = starField_->animationTime();
    if (lensFlare_) frame.flareIntensity = lensFlare_->getIntensity();
    return frame;
}

void SkySystem::record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                        const Camera& camera, const SkyParams& params, const Frame& frame) {
    if (!initialized_) {
        return;
    }

    // --- Skybox (authoritative sky gradient, DBC-driven colors) ---
    if (skybox_ && frame.sky) {
        skybox_->render(cmd, perFrameSet, params);
    }

    // Original client sky M2s supply their own celestial bodies, clouds, and
    // nebula layers. The caller draws that model over the gradient underlay.

    // --- Procedural stars ---
    // Not only a fallback any more: with sharp stars on, the sky model keeps
    // its clouds and planets while M2Renderer drops its star layer, and these
    // are drawn in that layer's place. So the original-skybox path continues
    // here rather than returning, and skyboxHasStars stops meaning "do not".
    bool renderProceduralStars = false;
    if (frame.debugSky) {
        renderProceduralStars = true;
    } else if (frame.proceduralStars) {
        renderProceduralStars = true;
    } else if (!params.useOriginalSkybox) {
        renderProceduralStars = !params.skyboxHasStars;
    }

    celestialSkipped_ = params.useOriginalSkybox && !renderProceduralStars;
    if (celestialSkipped_) {
        return;
    }

    if (starField_) {
        if (renderProceduralStars) {
            const float cloudDensity = params.cloudDensity;
            const float fogDensity   = params.fogDensity;
            starField_->render(cmd, perFrameSet, params.timeOfDay, cloudDensity, fogDensity, frame.twinkleTime);
        }
    }

    // --- The sun's disc, then the moon's, each on its own path (0x7f0b43) ---
    // Both in the sun's colour and put out by weather alike; each is gone when
    // its path takes it under the horizon.
    if (bodies_ && params.drawBodies) {
        const float weather = sky_celestial::weatherFade(params.weatherIntensity);
        bodies_->renderDisc(cmd, perFrameSet, SkyBodies::Body::Sun, params.sunDisc,
                            params.sunColor, weather);
        bodies_->renderDisc(cmd, perFrameSet, SkyBodies::Body::Moon, params.moonDisc,
                            params.sunColor, weather);
    }

    // --- Clouds (DBC-driven colors + sun lighting) ---
    if (clouds_ && frame.clouds) {
        // The cover LightParams gives, already blended toward the storm row by
        // the weather as the client blends it. A fixed 0.4 per unit of weather
        // was added on top, a second storm over the first.
        clouds_->render(cmd, perFrameSet, params, frame.cloudOffset);
    }

    // --- Lens flare: around the disc, as much of it as the glare shows ---
    if (lensFlare_ && frame.flare) {
        lensFlare_->render(cmd, camera, params.sunDisc.direction * 800.0f, params.sunColor,
                           params.sunGlareVisibility, frame.flareIntensity);
    }
}

void SkySystem::loadTextures(pipeline::AssetManager* assets) {
    if (bodies_) bodies_->loadTextures(assets);
}

} // namespace rendering
} // namespace wowee

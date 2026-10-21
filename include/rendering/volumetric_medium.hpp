#pragma once

#include "rendering/weather_mix.hpp"
#include <glm/glm.hpp>

namespace wowee::rendering {

// Scattering albedo of suspended sand, shared with the weather's distant haze.
inline const glm::vec3 kSandAlbedo{0.80f, 0.64f, 0.42f};

struct VolumetricMedium {
    float air = 0.0f;
    float mist = 0.0f;
    float dust = 0.0f;

    [[nodiscard]] float atGround() const { return air + mist + dust; }
};

[[nodiscard]] inline VolumetricMedium volumetricMediumFor(
    const WeatherMix& weather, float density, bool indoors) {
    // Fog distance is an artistic visibility control, not a humidity field.
    // Clear weather has only thin scattering air. Precipitation increases
    // suspended particles; only actual fog weather adds a dense low layer.
    // Coefficients are per yard, before the player's mist-density multiplier.
    const float scale = glm::clamp(density, 0.0f, 3.0f) * (indoors ? 0.35f : 1.0f);
    return {
        .air = (0.0002f + 0.0012f * weather.rain + 0.0008f * weather.snow) * scale,
        .mist = 0.012f * weather.mist * scale,
        .dust = 0.006f * weather.sand * scale,
    };
}

[[nodiscard]] inline VolumetricMedium blendVolumetricMedium(
    const VolumetricMedium& from, const VolumetricMedium& to, float amount) {
    return {glm::mix(from.air, to.air, amount), glm::mix(from.mist, to.mist, amount),
            glm::mix(from.dust, to.dust, amount)};
}

} // namespace wowee::rendering

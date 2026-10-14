#pragma once

// The point lights of a frame and which of them reach what.
//
// As the 3.3.5a client has it (WoW.exe 12340; build-dev notes
// m2_lights_findings.md):
//
// - The only point lights are the M2Lights of models animated this frame - a
//   lamp's flame, a brazier's glow. WMO and map lights never become point
//   lights.
// - Each receiver - a model, a WMO group, a terrain chunk - takes the lights
//   nearest its centre from the scene's 20-yard grid cells around it
//   (CM2Scene::GatherLights 0x81e400, AddLight 0x834f60). No range or
//   brightness test: the nearest, however far.
// - A light falls off as 1 / (0.7 d + 0.03 d^2) whatever its attenuation
//   tracks say; the client never reads them.
//
// One choice differs: the client takes the nearest four (three for terrain);
// here every receiver takes kLocalLightsPerReceiver. More lights means fewer
// seams where neighbouring chunks or groups picked different sets.
//
// Light indices travel to the shaders as a uvec4: eight 16-bit indices into
// the frame's light buffer, nearest first, 0xFFFF for none
// (local_lights.glsli).

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// A point light as the shaders take it: where it is and the colour it gives,
/// intensity and the owning model's alpha already folded in.
struct LocalLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{0.0f};
};

/// The lights that reach one receiver, packed two 16-bit indices per word.
using LocalLightSet = glm::uvec4;
inline constexpr uint32_t kNoLocalLightIndex = 0xFFFFu;
inline const LocalLightSet kNoLocalLights{0xFFFFFFFFu};

/// How many lights a receiver takes (the client: four, three for terrain).
inline constexpr int kLocalLightsPerReceiver = 8;
/// The most lights a frame can address; past it a light cannot be indexed.
inline constexpr uint32_t kMaxAddressableLocalLights = 0xFFFEu;
/// The scene grid's cell, in yards.
inline constexpr float kLocalLightCellSize = 20.0f;

/// The constant, linear and quadratic attenuation every M2 light keeps.
inline constexpr float kLocalLightAttenConstant = 0.0f;
inline constexpr float kLocalLightAttenLinear = 0.7f;
inline constexpr float kLocalLightAttenQuadratic = 0.03f;

inline float localLightAttenuation(float distance) {
    const float denom = kLocalLightAttenConstant + kLocalLightAttenLinear * distance +
                        kLocalLightAttenQuadratic * distance * distance;
    return denom > 1e-4f ? 1.0f / denom : 1e4f;
}

/// The cells a receiver at `center` with bounding radius `radius` gathers
/// from, along one axis: floor((c - r)/20 - 0.5) .. floor((c + r)/20 + 0.5).
inline std::array<int, 2> localLightCellRange(float center, float radius) {
    return {static_cast<int>(std::floor((center - radius) / kLocalLightCellSize - 0.5f)),
            static_cast<int>(std::floor((center + radius) / kLocalLightCellSize + 0.5f))};
}

/// Slot `k` of a packed set: an index into the frame's lights, or
/// kNoLocalLightIndex.
inline uint32_t localLightSlot(const LocalLightSet& set, int k) {
    return (set[k >> 1] >> (16 * (k & 1))) & 0xFFFFu;
}

/// The lights that reach a receiver, packed for a shader: up to `maxCount`
/// indices into `lights`, nearest to `center` first.
inline LocalLightSet pickLocalLights(const std::vector<LocalLight>& lights, const glm::vec3& center,
                                     float radius, int maxCount = kLocalLightsPerReceiver) {
    const auto rx = localLightCellRange(center.x, radius);
    const auto ry = localLightCellRange(center.y, radius);
    struct Near { float distSq; uint32_t index; };
    std::array<Near, kLocalLightsPerReceiver> best{};
    int count = 0;
    const int cap = std::clamp(maxCount, 0, kLocalLightsPerReceiver);
    const size_t addressable = std::min<size_t>(lights.size(), kMaxAddressableLocalLights);
    for (uint32_t i = 0; i < addressable; ++i) {
        const glm::vec3& p = lights[i].position;
        const int cx = static_cast<int>(std::floor(p.x / kLocalLightCellSize));
        const int cy = static_cast<int>(std::floor(p.y / kLocalLightCellSize));
        if (cx < rx[0] || cx > rx[1] || cy < ry[0] || cy > ry[1]) continue;
        const glm::vec3 d = p - center;
        const Near n{glm::dot(d, d), i};
        // Insertion into the sorted few, as AddLight keeps them.
        int at = count;
        while (at > 0 && best[at - 1].distSq > n.distSq) --at;
        if (at >= cap) continue;
        for (int k = std::min(count, cap - 1); k > at; --k) best[k] = best[k - 1];
        best[at] = n;
        count = std::min(count + 1, cap);
    }
    LocalLightSet packed = kNoLocalLights;
    for (int k = 0; k < count; ++k) {
        const uint32_t shift = 16u * static_cast<uint32_t>(k & 1);
        packed[k >> 1] = (packed[k >> 1] & ~(0xFFFFu << shift)) | ((best[k].index & 0xFFFFu) << shift);
    }
    return packed;
}

}  // namespace wowee::rendering

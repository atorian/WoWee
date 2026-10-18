#pragma once

/// How the Light.dbc volumes around a point make its light, as WoW.exe 3.3.5a
/// makes it (build 12340, 0x7f3230 and the loop 0x7f1360).
///
/// The client starts from the map's global light - the map's Light.dbc row at
/// the origin, with no extent - and lays every volume the point stands in over
/// it, one at a time, each as `light = lerp(light, volume, weight)`
/// (0x7ee5d0, 0x7ed4c0):
///
/// - The volumes are those whose outer sphere holds the point, less any whose
///   outer radius is three yards or less (0x7f1434).
/// - A volume weighs 1 inside its inner radius and falls linearly to 0 at its
///   outer one (0x7ee62b).
/// - They are laid down farthest centre first, so the nearest comes last and
///   is what the point mostly sees: a heap on the squared distance to the
///   centre, popped largest first (0x7f0dc0, 0x7f1280, compare 0x7ed0a0). Two
///   volumes whose centres are within a third of a yard of each other go by
///   inner radius instead, the larger first, so the tighter one is laid over
///   it.
///
/// So at a volume's edge its light gives way to whatever lies under it, and at
/// the last edge to the map's own light. Nothing in it is ever renormalised:
/// a volume at a tenth of its weight is a tenth of the light.
///
/// The functions here are the rule apart from any loaded file, so it can be
/// held against a table of cases.

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace wowee::rendering {

/// Volumes this small take no part, whatever their weight (0x7f1434: 3.0).
inline constexpr float kLightVolumeMinOuterRadius = 3.0f;

/// Centres closer than this are the same place, and their volumes are ordered
/// by inner radius rather than distance (0x7ed110: 1/3).
inline constexpr float kLightVolumeSameCentre = 1.0f / 3.0f;

/// The weight a volume gives a point `distance` from its centre: 1 within the
/// inner radius, falling linearly to 0 at the outer one.
inline float lightVolumeWeight(float distance, float innerRadius, float outerRadius) {
    if (distance <= innerRadius) return 1.0f;
    if (outerRadius <= innerRadius) return 0.0f;
    return glm::clamp(1.0f - (distance - innerRadius) / (outerRadius - innerRadius), 0.0f, 1.0f);
}

/// One volume around the point, for ordering.
struct LightVolumePlace {
    glm::vec3 centre{0.0f};
    float distanceSq = 0.0f;  ///< from the point to the centre
    float innerRadius = 0.0f;
    uint32_t id = 0;
};

/// True when `a` is laid down before `b` - that is, when `b` is laid over it.
///
/// Light id decides what the client leaves to its heap, so that two volumes
/// of the same extent at the same distance cannot swap places between frames.
inline bool lightVolumeLaidBefore(const LightVolumePlace& a, const LightVolumePlace& b) {
    if (glm::distance(a.centre, b.centre) <= kLightVolumeSameCentre) {
        if (a.innerRadius != b.innerRadius) return a.innerRadius > b.innerRadius;
    } else if (a.distanceSq != b.distanceSq) {
        return a.distanceSq > b.distanceSq;
    }
    return a.id < b.id;
}

/// What each layer ends up contributing once all of them are laid down.
///
/// `weights` are the volumes' own weights in the order they are laid. Each
/// lerp keeps (1 - w) of everything under it, so a volume's share is its
/// weight times what the ones laid over it leave, and the map's global light
/// keeps what is left after all of them.
struct LightVolumeShares {
    std::vector<float> volumes;  ///< one per weight, in the same order
    float global = 1.0f;
};

inline LightVolumeShares lightVolumeShares(std::span<const float> weights) {
    LightVolumeShares out;
    out.volumes.assign(weights.size(), 0.0f);
    for (size_t i = 0; i < weights.size(); ++i) {
        const float w = glm::clamp(weights[i], 0.0f, 1.0f);
        for (size_t k = 0; k < i; ++k) out.volumes[k] *= 1.0f - w;
        out.global *= 1.0f - w;
        out.volumes[i] = w;
    }
    return out;
}

}  // namespace wowee::rendering

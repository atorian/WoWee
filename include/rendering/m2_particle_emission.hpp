#pragma once

// How an M2 particle emitter emits, as the 3.3.5a client does it
// (CParticleEmitter2 in WoW.exe 12340, read from the disassembly - see
// docs/wow-client-render-spec.md, sections 4 and 6.1). Plain arithmetic, kept apart
// from the renderer so it can be checked against those numbers.

#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

/// Emission thins with the emitter's distance from the camera: full rate to
/// 50 yards, falling 2% a yard to a quarter from 87.5 yards on (0x97d8c0).
inline float m2EmissionDistanceLod(float distanceYards) {
    return std::clamp(1.0f - (distanceYards - 50.0f) * 0.02f, 0.25f, 1.0f);
}

/// Particles a second: the authored rate give or take its variation (one
/// random draw in [-1, 1] per emitter per frame), times the particle density
/// setting and the distance LOD. The density thins every emitter alike -
/// flames included; it defaults to 1.
inline float m2EmitterRate(float rate, float rateVary, float randomSigned,
                           float density, float distanceLod) {
    return std::max(0.0f, rate + rateVary * randomSigned) * density * distanceLod;
}

/// How many particles an emitter may hold: its most rate times its longest
/// life, with 15% to spare (0x97edf0). The client keeps the largest pool an
/// emitter has needed and never shrinks it; a full pool stops emission until
/// a particle dies, recycling none.
inline uint32_t m2EmitterPool(float rate, float rateVary, float life, float lifeVary) {
    const float n = (std::max(0.0f, rate) + std::max(0.0f, rateVary)) *
                    (std::max(0.0f, life) + std::max(0.0f, lifeVary)) * 1.15f;
    return n > 0.0f ? static_cast<uint32_t>(n) : 0u;
}

/// One particle's life, never below a millisecond. The life variation only
/// sizes the pool: the plane, sphere and spline spawners never write the
/// per-particle seed it is scaled by, so every particle of an emitter lives
/// the same time (section 4.5).
inline float m2ParticleLife(float life) {
    return std::max(life, 0.001f);
}

/// Where an FBlock is at a fraction t of a particle's life: between keys
/// `from` and `to`, `frac` of the way (0x9793b0). Linear, with no
/// extrapolation. Two keys ignore their timestamps; three use only the
/// middle one; more are searched.
struct M2FBlockSpan {
    size_t from = 0;
    size_t to = 0;
    float frac = 0.0f;
};
inline M2FBlockSpan m2FBlockSpan(const std::vector<float>& timestamps, size_t keyCount, float t) {
    if (keyCount <= 1) return {};
    if (keyCount == 2) return {0, 1, t};
    if (keyCount == 3) {
        const float m = timestamps.size() > 1 ? timestamps[1] : 0.5f;
        if (t < m) return {0, 1, m > 0.0f ? t / m : 0.0f};
        return {1, 2, m < 1.0f ? (t - m) / (1.0f - m) : 0.0f};
    }
    const size_t n = std::min(keyCount, timestamps.size());
    if (n < 2 || t <= timestamps[0]) return {0, 0, 0.0f};
    if (t >= timestamps[n - 1]) return {n - 1, n - 1, 0.0f};
    const size_t to = static_cast<size_t>(
        std::upper_bound(timestamps.begin(), timestamps.begin() + n, t) - timestamps.begin());
    const size_t from = to - 1;
    const float span = timestamps[to] - timestamps[from];
    return {from, to, span > 0.0f ? (t - timestamps[from]) / span : 0.0f};
}

/// The emitter's own space into its bone's: a quarter turn about Z, the same
/// for every emitter (CM2Model::UpdateParticleEmitter, 0x830c3c; a constant
/// built at 0xd411e0). Emitter X is bone +Y, emitter Y bone -X. Spawn points
/// and directions go through it before the emitter's offset and the bone; a
/// mage portal's ring, laid in the emitter's XZ plane, stands in the portal's
/// plane only with it, and tumbled through every angle without.
inline glm::vec3 m2EmitterToBone(const glm::vec3& v) {
    return {-v.y, v.x, v.z};
}

/// A plane emitter's spawn point, in the emitter's space: anywhere across its
/// area, whose length (x) and width (y) are full extents.
inline glm::vec3 m2PlaneSpawnOffset(float length, float width, float randX, float randY) {
    return {randX * length * 0.5f, randY * width * 0.5f, 0.0f};
}

/// A plane emitter's direction: tilted from +Z by `polar`, turned about it by
/// `azimuth` from +X. Both are the emitter's ranges times a draw in [-1, 1].
inline glm::vec3 m2ConeDirection(float polar, float azimuth) {
    return {std::sin(polar) * std::cos(azimuth), std::sin(polar) * std::sin(azimuth),
            std::cos(polar)};
}

/// A sphere emitter's spawn point: between the minimum (area length) and
/// maximum (area width) radius, `elevation` up from the XY plane and
/// `azimuth` round from +X.
inline glm::vec3 m2SphereSpawnOffset(float minRadius, float maxRadius, float rand01,
                                     float elevation, float azimuth) {
    const float r = minRadius + rand01 * (maxRadius - minRadius);
    return r * glm::vec3(std::cos(elevation) * std::cos(azimuth),
                         std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
}

/// The direction away from a zSource: from (0, 0, zSource) through the spawn
/// point. A spawn on that point has none.
inline glm::vec3 m2ZSourceDirection(const glm::vec3& spawnOffset, float zSource) {
    const glm::vec3 d = spawnOffset - glm::vec3(0.0f, 0.0f, zSource);
    const float len = glm::length(d);
    return len > 0.0f ? d / len : glm::vec3(0.0f);
}

/// One step of a particle's flight (0x979bb0): wind while it is young enough,
/// gravity along -Z integrated exactly over the step, then drag.
inline void m2ParticleStep(glm::vec3& position, glm::vec3& velocity, float age, float dt,
                           float gravity, float drag, const glm::vec3& wind, float windTime) {
    if (age < windTime) velocity += wind * dt;
    position.x += velocity.x * dt;
    position.y += velocity.y * dt;
    position.z += velocity.z * dt - 0.5f * gravity * dt * dt;
    velocity.z -= gravity * dt;
    velocity -= velocity * std::min(1.0f, drag * dt);
}

/// A particle's own size factor from the emitter's scale variation, fixed for
/// its life: one draw for both axes, or one each with flag 0x80000.
inline glm::vec2 m2ScaleVaryFactor(const glm::vec2& vary, bool independent,
                                   float randX, float randY) {
    const float x = std::max(1e-4f, 1.0f + randX * vary.x);
    const float y = independent ? std::max(1e-4f, 1.0f + randY * vary.y) : x;
    return {x, y};
}

/// The spin of a particle's quad, radians counter-clockwise in the view plane.
/// None unless the emitter spins: a base angle alone is not applied.
inline float m2ParticleSpin(float baseSpin, float baseSpinVary, float spin, float spinVary,
                            float randBase, float randSpin, float age) {
    if (spin == 0.0f && spinVary == 0.0f) return 0.0f;
    return baseSpin + randBase * baseSpinVary + age * (spin + randSpin * spinVary);
}

/// Below this squared speed a velocity-aligned head (flag 0x4) is laid out
/// as an ordinary one.
constexpr float kM2AlongVelocityMinSpeedSq = 2.4e-7f;

/// The twinkle table's slot for a particle: its age at the twinkle speed,
/// offset by a slot of its own (the client uses the particle's address).
constexpr uint32_t kM2TwinkleSlots = 128;

/// The twinkle table: 128 fractions in [0, 1), drawn once.
inline const std::array<float, kM2TwinkleSlots>& m2TwinkleTable() {
    static const std::array<float, kM2TwinkleSlots> table = [] {
        std::array<float, kM2TwinkleSlots> t{};
        uint32_t state = 0x12345678u;
        for (float& v : t) {
            state = state * 1664525u + 1013904223u;
            v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        }
        return t;
    }();
    return table;
}
inline uint32_t m2TwinkleSlot(float twinkleSpeed, float age, uint32_t particleOffset) {
    return (static_cast<uint32_t>(static_cast<int32_t>(twinkleSpeed * age)) + particleOffset) &
           (kM2TwinkleSlots - 1);
}

/// Particles to spawn this frame from the accumulated fraction: rounded, not
/// floored, and what is spawned is taken off the accumulator afterwards.
inline int m2SpawnCount(float accumulated) {
    return accumulated > 0.0f ? static_cast<int>(std::lround(accumulated)) : 0;
}

/// Longest step a particle simulation takes: a longer frame is played in
/// steps of this, at most one lifespan's worth (0x97acb0).
constexpr float kM2ParticleMaxStep = 0.1f;

/// Most particles one emitter draws in a frame: 0x4000 vertices, four to a
/// quad, so 4096 heads, or 2048 when each particle is a head and a tail.
/// Particles past it are simulated but not drawn.
inline uint32_t m2EmitterDrawCap(bool head, bool tail) {
    return (head && tail) ? 2048u : 4096u;
}

} // namespace rendering
} // namespace wowee

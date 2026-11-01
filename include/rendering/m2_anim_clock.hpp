#pragma once

#include <cmath>

namespace wowee {
namespace rendering {

/// The period an instance with emitters and no sequence of its own loops at,
/// so its emission rate and lifespan tracks keep cycling. Long enough to cover
/// the longest known emission cycle (~3 s for torches and campfires).
inline constexpr float kM2ParticleWrapMs = 3333.0f;

/// The sequence time an M2 instance shows when the renderer's animation clock
/// reads clockMs.
///
/// An instance keeps the clock reading at which its sequence was at zero rather
/// than a running total, so one that nothing steps costs nothing per frame: its
/// time is worked out when something reads it. Most of a city's doodads are
/// like that - a texture that scrolls, an emitter that breathes - and advancing
/// a total for each of them was a pass over every instance every frame.
///
/// A stepped instance (one with a skeleton the update loop plays) is wrapped by
/// that loop, which also holds it at the end or switches its sequence, so its
/// time is returned as it is. One that is not stepped loops at its sequence
/// length, or, with emitters and no sequence, at kM2ParticleWrapMs.
///
/// The clock is a double: a float holding hours of milliseconds has lost the
/// fraction of a frame that keeps a scroll smooth.
inline float m2AnimTimeAt(double clockMs, double startMs, float durationMs,
                          bool stepped, bool particleOnly) {
    const double t = clockMs - startMs;
    if (stepped) return static_cast<float>(t);
    if (durationMs > 0.0f) return static_cast<float>(std::fmod(t, static_cast<double>(durationMs)));
    if (particleOnly) return static_cast<float>(std::fmod(t, static_cast<double>(kM2ParticleWrapMs)));
    return static_cast<float>(t);
}

} // namespace rendering
} // namespace wowee

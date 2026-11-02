#pragma once

// How much room an M2 particle emitter needs, as the 3.3.5a client sizes it
// (CParticleEmitter2 in WoW.exe 12340, read from the disassembly - see
// docs/wow-client-render-spec.md, sections 4 and 6.1). The rest of the
// client's emission and flight runs on the GPU (m2_particles.comp.glsl),
// which sizes its pool by the same rule.

#include <algorithm>
#include <cstdint>

namespace wowee {
namespace rendering {

/// How many particles an emitter may hold: its most rate times its longest
/// life, with 15% to spare (0x97edf0). The client keeps the largest pool an
/// emitter has needed and never shrinks it; a full pool stops emission until
/// a particle dies, recycling none.
inline uint32_t m2EmitterPool(float rate, float rateVary, float life, float lifeVary) {
    const float n = (std::max(0.0f, rate) + std::max(0.0f, rateVary)) *
                    (std::max(0.0f, life) + std::max(0.0f, lifeVary)) * 1.15f;
    return n > 0.0f ? static_cast<uint32_t>(n) : 0u;
}

} // namespace rendering
} // namespace wowee

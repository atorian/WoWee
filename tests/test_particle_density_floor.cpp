// How much room an M2 particle emitter needs, against the 3.3.5a client's
// own arithmetic (CParticleEmitter2, docs/wow-client-render-spec.md section
// 4). The emission and flight themselves run on the GPU and are checked
// there (tools/scene_render/check_m2_particles.py).
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_emission.hpp"

using namespace wowee::rendering;

TEST_CASE("An emitter holds its rate times its life, and 15% more", "[particles][emission]") {
    // A Dalaran mage portal's longest swirl: 500/s for 5.5 s.
    CHECK(m2EmitterPool(500.0f, 0.0f, 5.5f, 0.0f) == 3162u);
    // The variations widen it.
    CHECK(m2EmitterPool(10.0f, 2.0f, 1.0f, 0.5f) == 20u);
    CHECK(m2EmitterPool(0.0f, 0.0f, 3.0f, 0.0f) == 0u);
}

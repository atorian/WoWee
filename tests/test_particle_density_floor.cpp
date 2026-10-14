// M2 particle emission against the 3.3.5a client's own arithmetic
// (CParticleEmitter2, docs/wow-client-render-spec.md section 4).
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_emission.hpp"

#include <glm/gtc/constants.hpp>

using namespace wowee::rendering;

TEST_CASE("Emission thins with distance from 50 to 87.5 yards", "[particles][emission]") {
    CHECK(m2EmissionDistanceLod(0.0f) == Catch::Approx(1.0f));
    CHECK(m2EmissionDistanceLod(50.0f) == Catch::Approx(1.0f));
    CHECK(m2EmissionDistanceLod(70.0f) == Catch::Approx(0.6f));
    CHECK(m2EmissionDistanceLod(87.5f) == Catch::Approx(0.25f));
    CHECK(m2EmissionDistanceLod(300.0f) == Catch::Approx(0.25f));
}

TEST_CASE("The density setting thins every emitter, flames included", "[particles][emission]") {
    CHECK(m2EmitterRate(40.0f, 0.0f, 0.0f, 0.1f, 1.0f) == Catch::Approx(4.0f));
    // A Dalaran sconce's halo, 0.4/s, at the default density.
    CHECK(m2EmitterRate(0.4f, 0.0f, 0.0f, 1.0f, 1.0f) == Catch::Approx(0.4f));
    // The variation swings the rate both ways, never below nothing.
    CHECK(m2EmitterRate(10.0f, 4.0f, 1.0f, 1.0f, 1.0f) == Catch::Approx(14.0f));
    CHECK(m2EmitterRate(10.0f, 4.0f, -1.0f, 1.0f, 1.0f) == Catch::Approx(6.0f));
    CHECK(m2EmitterRate(1.0f, 4.0f, -1.0f, 1.0f, 1.0f) == Catch::Approx(0.0f));
    // And distance thins it further.
    CHECK(m2EmitterRate(500.0f, 0.0f, 0.0f, 1.0f, m2EmissionDistanceLod(100.0f)) ==
          Catch::Approx(125.0f));
}

TEST_CASE("An emitter holds its rate times its life, and 15% more", "[particles][emission]") {
    // A Dalaran mage portal's longest swirl: 500/s for 5.5 s.
    CHECK(m2EmitterPool(500.0f, 0.0f, 5.5f, 0.0f) == 3162u);
    // The variations widen it.
    CHECK(m2EmitterPool(10.0f, 2.0f, 1.0f, 0.5f) == 20u);
    CHECK(m2EmitterPool(0.0f, 0.0f, 3.0f, 0.0f) == 0u);
}

TEST_CASE("Every particle of an emitter lives its life, at least a millisecond", "[particles][emission]") {
    CHECK(m2ParticleLife(3.0f) == Catch::Approx(3.0f));
    CHECK(m2ParticleLife(0.0f) == Catch::Approx(0.001f));
}

TEST_CASE("FBlocks are read as the client reads them", "[particles][fblock]") {
    // Two keys: straight across the life, whatever the timestamps say.
    M2FBlockSpan two = m2FBlockSpan({0.0f, 0.3f}, 2, 0.6f);
    CHECK(two.from == 0);
    CHECK(two.to == 1);
    CHECK(two.frac == Catch::Approx(0.6f));
    // Three keys: only the middle timestamp counts. Shadowform's base glow,
    // keys at 0, 0.5 and 1 of its life.
    M2FBlockSpan early = m2FBlockSpan({0.0f, 0.5f, 0.9f}, 3, 0.25f);
    CHECK(early.from == 0);
    CHECK(early.to == 1);
    CHECK(early.frac == Catch::Approx(0.5f));
    M2FBlockSpan late = m2FBlockSpan({0.0f, 0.5f, 0.9f}, 3, 0.75f);
    CHECK(late.from == 1);
    CHECK(late.to == 2);
    CHECK(late.frac == Catch::Approx(0.5f));
    // More: searched. A 4x4 flipbook's cells, 0 to 7 then 8 to 15.
    const std::vector<float> cells{0.0f, 0.5f, 0.5f, 1.0f};
    M2FBlockSpan firstHalf = m2FBlockSpan(cells, 4, 0.25f);
    CHECK(firstHalf.from == 0);
    CHECK(firstHalf.to == 1);
    CHECK(firstHalf.frac == Catch::Approx(0.5f));
    M2FBlockSpan secondHalf = m2FBlockSpan(cells, 4, 0.75f);
    CHECK(secondHalf.from == 2);
    CHECK(secondHalf.to == 3);
    CHECK(secondHalf.frac == Catch::Approx(0.5f));
    // One key: constant.
    CHECK(m2FBlockSpan({0.0f}, 1, 0.7f).to == 0);
}

TEST_CASE("Emitters place particles as the client does", "[particles][emission]") {
    // A plane's length and width are full extents.
    const glm::vec3 corner = m2PlaneSpawnOffset(2.0f, 4.0f, 1.0f, -1.0f);
    CHECK(corner.x == Catch::Approx(1.0f));
    CHECK(corner.y == Catch::Approx(-2.0f));
    CHECK(corner.z == Catch::Approx(0.0f));
    // A sphere's are its least and greatest radius.
    const glm::vec3 inner = m2SphereSpawnOffset(0.5f, 1.5f, 0.0f, 0.0f, 0.0f);
    CHECK(inner.x == Catch::Approx(0.5f));
    const glm::vec3 top = m2SphereSpawnOffset(0.5f, 1.5f, 1.0f, glm::half_pi<float>(), 0.0f);
    CHECK(top.z == Catch::Approx(1.5f));
    // The emitter's space is a quarter turn from its bone's: a ring laid in
    // the emitter's XZ plane stands in the bone's YZ plane.
    const glm::vec3 ringTop = m2EmitterToBone(m2SphereSpawnOffset(2.0f, 2.0f, 0.0f, 0.0f, 0.0f));
    CHECK(ringTop.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(ringTop.y == Catch::Approx(2.0f));
    CHECK(m2EmitterToBone(glm::vec3(0.0f, 1.0f, 0.5f)).x == Catch::Approx(-1.0f));
    CHECK(m2EmitterToBone(glm::vec3(0.0f, 1.0f, 0.5f)).z == Catch::Approx(0.5f));
    // Straight up with no tilt; a zSource sends them away from it.
    CHECK(m2ConeDirection(0.0f, 1.0f).z == Catch::Approx(1.0f));
    const glm::vec3 fromBelow = m2ZSourceDirection(glm::vec3(1.0f, 0.0f, 0.0f), -1.0f);
    CHECK(fromBelow.x == Catch::Approx(std::sqrt(0.5f)));
    CHECK(fromBelow.z == Catch::Approx(std::sqrt(0.5f)));
}

TEST_CASE("A particle flies under gravity, drag and wind", "[particles][update]") {
    glm::vec3 pos(0.0f), vel(0.0f, 0.0f, 2.0f);
    m2ParticleStep(pos, vel, 0.0f, 0.1f, 10.0f, 0.0f, glm::vec3(0.0f), 0.0f);
    CHECK(pos.z == Catch::Approx(0.2f - 0.05f));
    CHECK(vel.z == Catch::Approx(1.0f));
    // Drag takes its share of the velocity, never more than all of it.
    glm::vec3 p2(0.0f), v2(4.0f, 0.0f, 0.0f);
    m2ParticleStep(p2, v2, 0.0f, 0.1f, 0.0f, 2.5f, glm::vec3(0.0f), 0.0f);
    CHECK(v2.x == Catch::Approx(3.0f));
    m2ParticleStep(p2, v2, 0.0f, 0.1f, 0.0f, 50.0f, glm::vec3(0.0f), 0.0f);
    CHECK(v2.x == Catch::Approx(0.0f));
    // Wind only while the particle is young.
    glm::vec3 p3(0.0f), v3(0.0f);
    m2ParticleStep(p3, v3, 0.5f, 0.1f, 0.0f, 0.0f, glm::vec3(10.0f, 0.0f, 0.0f), 1.0f);
    CHECK(v3.x == Catch::Approx(1.0f));
    m2ParticleStep(p3, v3, 1.5f, 0.1f, 0.0f, 0.0f, glm::vec3(10.0f, 0.0f, 0.0f), 1.0f);
    CHECK(v3.x == Catch::Approx(1.0f));
}

TEST_CASE("Particle size and spin vary as the client's do", "[particles][size]") {
    const glm::vec2 same = m2ScaleVaryFactor(glm::vec2(0.5f, 0.9f), false, 1.0f, -1.0f);
    CHECK(same.x == Catch::Approx(1.5f));
    CHECK(same.y == Catch::Approx(1.5f));
    const glm::vec2 apart = m2ScaleVaryFactor(glm::vec2(0.5f, 0.9f), true, 1.0f, -1.0f);
    CHECK(apart.y == Catch::Approx(0.1f));
    CHECK(m2ScaleVaryFactor(glm::vec2(2.0f), false, -1.0f, 0.0f).x == Catch::Approx(1e-4f));
    // A base angle alone does not turn a particle.
    CHECK(m2ParticleSpin(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f) == Catch::Approx(0.0f));
    CHECK(m2ParticleSpin(1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 2.0f) == Catch::Approx(2.0f));
    // The twinkle table holds fractions, and its slot wraps.
    for (float v : m2TwinkleTable()) {
        CHECK(v >= 0.0f);
        CHECK(v < 1.0f);
    }
    CHECK(m2TwinkleSlot(10.0f, 1.0f, 125u) == 7u);
}

TEST_CASE("Spawning rounds the accumulated fraction", "[particles][emission]") {
    CHECK(m2SpawnCount(0.4f) == 0);
    CHECK(m2SpawnCount(0.5f) == 1);
    CHECK(m2SpawnCount(2.6f) == 3);
    CHECK(m2SpawnCount(-0.3f) == 0);
}

TEST_CASE("An emitter draws at most 0x4000 vertices", "[particles][emission]") {
    CHECK(m2EmitterDrawCap(true, false) == 4096u);
    CHECK(m2EmitterDrawCap(false, true) == 4096u);
    CHECK(m2EmitterDrawCap(true, true) == 2048u);
}

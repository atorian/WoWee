// When a shadow map's cached cull still holds (shadow_cull_reuse.hpp).
//
// What matters: while the check says the cached cull holds, every caster the
// box test keeps against the map of the moment passed the cached cull, with
// its slack - so reusing it drops no shadow. And once the map has moved by
// more than the slack, the check says so.

#include <catch_amalgamated.hpp>

#include "rendering/shadow_cull_reuse.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <random>

using wowee::rendering::lightSpaceCullStillCovers;

namespace {

/// The sun's map as Renderer::computeLightSpaceMatrix builds it.
glm::mat4 sunMap(glm::vec3 centre, glm::vec3 rays) {
    constexpr float kHalf = 500.0f;
    rays = glm::normalize(rays);
    glm::mat4 projection = glm::ortho(-kHalf, kHalf, -kHalf, kHalf, 1.0f, kHalf * 6.5f);
    projection[1][1] *= -1.0f;
    return projection * glm::lookAt(centre - rays * (kHalf * 3.0f), centre, glm::vec3(0, 0, 1));
}

/// The M2 shadow pass's test (M2Renderer::renderShadow): the position in the
/// map's clip box, widened by the caster's margin, plus any slack.
bool passes(const glm::mat4& map, glm::vec3 p, float margin, float slack) {
    const glm::vec4 c = map * glm::vec4(p, 1.0f);
    const float m = margin + slack;
    return std::abs(c.x) <= 1.0f + m && std::abs(c.y) <= 1.0f + m && c.z >= -m && c.z <= 1.0f + m;
}

constexpr float kSlack = 0.05f;
constexpr float kMaxMargin = 0.2f;
const glm::vec3 kDalaran(5804.0f, 624.0f, 647.0f);

}  // namespace

TEST_CASE("The same map, or one a texel step and a moment of sun away, keeps its cull",
          "[shadow_cull_reuse]") {
    const glm::vec3 rays(-0.45f, 0.35f, -0.55f);
    const glm::mat4 cached = sunMap(kDalaran, rays);
    CHECK(lightSpaceCullStillCovers(cached, cached, {-1, -1, 0}, {1, 1, 1}, kMaxMargin, kSlack));
    // A quarter-yard step, as the texel snap makes, and the sun a hundredth of a
    // degree on.
    const glm::vec3 turned = glm::vec3(glm::rotate(glm::mat4(1.0f), glm::radians(0.01f), glm::vec3(0, 0, 1)) *
                                       glm::vec4(rays, 0.0f));
    const glm::mat4 now = sunMap(kDalaran + glm::vec3(0.25f, 0.0f, 0.0f), turned);
    CHECK(lightSpaceCullStillCovers(cached, now, {-1, -1, 0}, {1, 1, 1}, kMaxMargin, kSlack));
}

TEST_CASE("A map moved by more than the slack asks for a new cull", "[shadow_cull_reuse]") {
    const glm::vec3 rays(-0.45f, 0.35f, -0.55f);
    const glm::mat4 cached = sunMap(kDalaran, rays);
    // Slack is in clip units; 0.05 of a 500-yard half extent is 25 yards across
    // the rays. Sixty yards along x is some 37 of them.
    CHECK_FALSE(lightSpaceCullStillCovers(cached, sunMap(kDalaran + glm::vec3(60, 0, 0), rays),
                                          {-1, -1, 0}, {1, 1, 1}, kMaxMargin, kSlack));
    const glm::vec3 turned = glm::vec3(glm::rotate(glm::mat4(1.0f), glm::radians(8.0f), glm::vec3(0, 0, 1)) *
                                       glm::vec4(rays, 0.0f));
    CHECK_FALSE(lightSpaceCullStillCovers(cached, sunMap(kDalaran, turned),
                                          {-1, -1, 0}, {1, 1, 1}, kMaxMargin, kSlack));
}

TEST_CASE("While the cull holds, no caster the new map keeps was outside the cached one",
          "[shadow_cull_reuse]") {
    const glm::vec3 rays(0.2f, -0.6f, -0.75f);
    const glm::mat4 cached = sunMap(kDalaran, rays);
    std::mt19937 rng(7);
    // Enough movement that some maps leave the slack, and the points that tell
    // the difference are there to be found.
    std::uniform_real_distribution<float> step(-30.0f, 30.0f);
    std::uniform_real_distribution<float> angle(-1.5f, 1.5f);
    std::uniform_real_distribution<float> spread(-1300.0f, 1300.0f);
    std::uniform_real_distribution<float> margin(0.0f, kMaxMargin);
    int heldMaps = 0;
    for (int trial = 0; trial < 200; ++trial) {
        const glm::vec3 turned = glm::vec3(
            glm::rotate(glm::mat4(1.0f), glm::radians(angle(rng)), glm::vec3(0, 0, 1)) * glm::vec4(rays, 0.0f));
        const glm::mat4 now = sunMap(kDalaran + glm::vec3(step(rng), step(rng), 0.0f), turned);
        if (!lightSpaceCullStillCovers(cached, now, {-1, -1, 0}, {1, 1, 1}, kMaxMargin, kSlack)) continue;
        ++heldMaps;
        for (int i = 0; i < 4000; ++i) {
            const glm::vec3 p = kDalaran + glm::vec3(spread(rng), spread(rng), spread(rng) * 0.3f);
            const float m = margin(rng);
            if (passes(now, p, m, 0.0f)) {
                INFO("trial " << trial << " point " << i);
                REQUIRE(passes(cached, p, m, kSlack));
            }
        }
    }
    CHECK(heldMaps > 10);
    CHECK(heldMaps < 190);
}

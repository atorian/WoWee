// What lies under the water, as the sea bed's map has it (SeaBedMap): its
// views look straight down on the ground and buildings under the water and
// give their heights back, a quay is the bed and a ship's hull is not
// (sea_bed.glsli, which the combining pass runs).
#include <catch_amalgamated.hpp>

#include "rendering/glsl_as_cpp.hpp"
#include "rendering/sea_bed_frame.hpp"

namespace wowee::rendering::glsl {
namespace {
#include "../assets/shaders/sea_bed.glsli"
}
}  // namespace wowee::rendering::glsl

using wowee::rendering::SeaBedFrame;
using wowee::rendering::seaBedFrameFor;
using wowee::rendering::glsl::seaBedDepth;

namespace {

constexpr float kHalf = 384.0f;
constexpr float kRange = 200.0f;
constexpr int kTexels = 1024;

glm::vec3 clipOf(const glm::mat4& m, glm::vec3 p) {
    const glm::vec4 c = m * glm::vec4(p, 1.0f);
    return glm::vec3(c) / c.w;
}

/// What a view's depth test keeps of several surfaces in one column: the
/// nearest to it, and only those inside its depth range.
float kept(const glm::mat4& m, glm::vec2 xy, std::initializer_list<float> heights) {
    float best = 1.0f;  // cleared
    for (float h : heights) {
        const float d = clipOf(m, glm::vec3(xy, h)).z;
        if (d >= 0.0f && d <= 1.0f) best = std::min(best, d);
    }
    return best;
}

}  // namespace

TEST_CASE("the sea bed's map looks straight down and gives each height back", "[water][sea_bed]") {
    const SeaBedFrame frame = seaBedFrameFor({-8574.7f, 1340.8f}, 0.0f, kHalf, kRange, kTexels);
    const glm::mat4 above = frame.viewProjection();
    for (glm::vec3 p : {glm::vec3(-8574.0f, 1341.0f, -3.0f), glm::vec3(-8300.0f, 1600.0f, -150.0f),
                        glm::vec3(-8900.0f, 1000.0f, -0.5f)}) {
        const glm::vec3 c = clipOf(above, p);
        CHECK(frame.heightAt(c.z) == Catch::Approx(p.z).margin(1e-3));
        // A point and one straight under it fall on the same texel.
        const glm::vec3 under = clipOf(above, p - glm::vec3(0.0f, 0.0f, 20.0f));
        CHECK(under.x == Catch::Approx(c.x).margin(1e-6));
        CHECK(under.y == Catch::Approx(c.y).margin(1e-6));
        // Inside the square, inside the map.
        CHECK(std::abs(c.x) <= 1.0f);
        CHECK(std::abs(c.y) <= 1.0f);
    }
    // Over the water is not under it, and nothing past the range's foot is.
    CHECK(clipOf(above, glm::vec3(frame.centre, 2.0f)).z < 0.0f);
    CHECK(clipOf(above, glm::vec3(frame.centre, -kRange - 1.0f)).z > 1.0f);
    // Outside the square, outside the map.
    CHECK(std::abs(clipOf(above, glm::vec3(frame.centre + glm::vec2(kHalf + 5.0f, 0.0f), -1.0f)).x) > 1.0f);
}

TEST_CASE("seen from below, the map keeps the lowest surface in a column", "[water][sea_bed]") {
    const SeaBedFrame frame = seaBedFrameFor({100.0f, 200.0f}, 10.0f, kHalf, kRange, kTexels);
    const glm::vec2 xy(120.0f, 180.0f);
    // A hull: its deck at the water, its keel four yards down.
    const float fromAbove = kept(frame.viewProjection(), xy, {10.0f, 6.0f});
    const float fromBelow = kept(frame.viewProjectionFromBelow(), xy, {10.0f, 6.0f});
    CHECK(frame.heightAt(fromAbove) == Catch::Approx(10.0f).margin(1e-3));
    CHECK((1.0f - fromBelow) * kRange == Catch::Approx(4.0f).margin(1e-3));  // yards under the water
    // Both views land the column on the same texel.
    const glm::vec3 a = clipOf(frame.viewProjection(), glm::vec3(xy, 5.0f));
    const glm::vec3 b = clipOf(frame.viewProjectionFromBelow(), glm::vec3(xy, 5.0f));
    CHECK(a.x == Catch::Approx(b.x).margin(1e-6));
    CHECK(a.y == Catch::Approx(b.y).margin(1e-6));
}

TEST_CASE("the map's texels stay put in the world as the camera moves", "[water][sea_bed]") {
    // Drawn again round a camera that has moved, every point of the shore
    // falls at the same place in its texel as before; the waves along it
    // would shimmer with every step otherwise.
    const auto withinTexel = [](const SeaBedFrame& f, glm::vec2 p) {
        const glm::vec3 c = clipOf(f.viewProjection(), glm::vec3(p, -1.0f));
        const glm::vec2 at = (glm::vec2(c) * 0.5f + 0.5f) * static_cast<float>(kTexels);
        return at - glm::floor(at);
    };
    const SeaBedFrame a = seaBedFrameFor({1000.1f, -50.2f}, 0.0f, kHalf, kRange, kTexels);
    for (glm::vec2 moved : {glm::vec2(0.2f, -0.1f), glm::vec2(13.37f, 7.9f), glm::vec2(-95.0f, 60.4f)}) {
        const SeaBedFrame b = seaBedFrameFor(glm::vec2(1000.1f, -50.2f) + moved, 0.0f, kHalf, kRange, kTexels);
        for (glm::vec2 shore : {glm::vec2(1010.3f, -40.7f), glm::vec2(1100.9f, -130.2f)}) {
            const glm::vec2 fa = withinTexel(a, shore), fb = withinTexel(b, shore);
            CHECK(fa.x == Catch::Approx(fb.x).margin(2e-3));
            CHECK(fa.y == Catch::Approx(fb.y).margin(2e-3));
        }
    }
}

TEST_CASE("a quay is the sea's bed, a ship's hull is not", "[water][sea_bed]") {
    // Depths as the views read them: from above 0 at the water, from below
    // 0 at the range's foot; 1 is nothing there.
    const auto fromAbove = [](float yardsUnder) { return yardsUnder / kRange; };
    const auto fromBelow = [](float yardsUnder) { return 1.0f - yardsUnder / kRange; };
    const float ground = fromAbove(15.0f);  // fifteen yards of harbour

    // Open water: the ground is the bed.
    CHECK(seaBedDepth(ground, 1.0f, 1.0f, kRange) == ground);
    // A quay wall from the water down into the ground: no water there.
    CHECK(seaBedDepth(ground, fromAbove(0.0f), fromBelow(16.0f), kRange) == fromAbove(0.0f));
    // A quay's foundation stopping just short of the ground it is set in.
    CHECK(seaBedDepth(ground, fromAbove(0.0f), fromBelow(14.5f), kRange) == fromAbove(0.0f));
    // A ship's hull, keel five yards down over fifteen: the harbour's depth.
    CHECK(seaBedDepth(ground, fromAbove(0.0f), fromBelow(5.0f), kRange) == ground);
    // A wreck on the bottom is the bottom.
    CHECK(seaBedDepth(ground, fromAbove(11.0f), fromBelow(15.0f), kRange) == fromAbove(11.0f));
    // A building's floor under the ground, where the terrain covers it,
    // changes nothing.
    CHECK(seaBedDepth(ground, fromAbove(20.0f), fromBelow(25.0f), kRange) == ground);
}

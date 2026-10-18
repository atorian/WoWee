// What lies under the water, as the sea bed's map has it (SeaBedMap): its
// views look straight down on the ground and buildings under the water and
// give their heights back, a quay is the bed and a ship's hull is not
// (sea_bed.glsli, which the combining pass runs).
#include <catch_amalgamated.hpp>

#include "rendering/glsl_as_cpp.hpp"
#include "rendering/sea_bed_frame.hpp"

#include <vector>

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

namespace {

/// A map of `n` texels a side, one yard each, and the effective fetch the
/// GPU finds over it on a grid of `n / 4` (sea_bed_fetch.comp): for each
/// radial, the shore read into a grid turned up it, the passes of 1, 2, 4
/// ... grid texels along its rows, then its share added into the map-aligned
/// sum. Read back at a map texel, as the water reads it.
struct Bay {
    int n = 0;
    std::vector<float> bed;  // depth as the views count it

    explicit Bay(int size) : n(size), bed(static_cast<size_t>(size) * size, 20.0f / kRange) {}
    void shore(glm::ivec2 p) {
        if (p.x >= 0 && p.y >= 0 && p.x < n && p.y < n) bed[p.y * n + p.x] = 0.0f;
    }
};

struct EffectiveFetch {
    int grid = 0;
    int n = 0;
    std::vector<glm::vec2> sums;

    /// As the water reads it: for inland water, or for the sea.
    float atMapTexel(glm::ivec2 texel, bool openSea = false) const {
        const glm::ivec2 g = texel * grid / n;
        return wowee::rendering::glsl::seaFetchFor(sums[g.y * grid + g.x], openSea, 1e6f);
    }
};

EffectiveFetch fetchByPasses(const Bay& bay, glm::vec2 toward) {
    using namespace wowee::rendering::glsl;
    const float open = 1e6f;
    const int n = bay.n, g = n / 4;
    const float texelYards = static_cast<float>(n) / g;
    EffectiveFetch f;
    f.grid = g;
    f.n = n;
    f.sums.assign(static_cast<size_t>(g) * g, glm::vec2(0.0f));
    std::vector<float> radialFetch(static_cast<size_t>(g) * g), next(radialFetch.size());
    for (int radial = 0; radial < kSeaFetchRadials; ++radial) {
        const vec2 upwind = seaFetchRadialUpwind(-toward, radial);
        for (int j = 0; j < g; ++j) {
            for (int i = 0; i < g; ++i) {
                const vec2 mapUv = seaFetchGridToMap((glm::vec2(i, j) + 0.5f) / static_cast<float>(g), upwind);
                float& out = radialFetch[j * g + i];
                out = open;
                if (mapUv.x < 0.0f || mapUv.y < 0.0f || mapUv.x >= 1.0f || mapUv.y >= 1.0f) continue;
                const int side = kSeaFetchShoreSamplesSide;
                int shore = 0;
                for (int sy = 0; sy < side; ++sy) {
                    for (int sx = 0; sx < side; ++sx) {
                        const glm::vec2 within =
                            (glm::vec2(i, j) + (glm::vec2(sx, sy) + 0.5f) / static_cast<float>(side)) /
                            static_cast<float>(g);
                        const glm::ivec2 at = glm::ivec2(seaFetchGridToMap(within, upwind) * static_cast<float>(n));
                        const int cx = std::clamp(at.x, 0, n - 1), cy = std::clamp(at.y, 0, n - 1);
                        if (seaBedIsShore(bay.bed[cy * n + cx], kRange)) ++shore;
                    }
                }
                out = seaBedFetchStart(seaFetchCellIsShore(shore, side * side), open);
            }
        }
        for (int step = 1; step < g; step *= 2) {
            for (int j = 0; j < g; ++j) {
                for (int i = 0; i < g; ++i) {
                    const float there = i + step < g ? radialFetch[j * g + i + step] : open;
                    next[j * g + i] = seaBedFetchPass(radialFetch[j * g + i], there, step * texelYards);
                }
            }
            radialFetch.swap(next);
        }
        for (int y = 0; y < g; ++y) {
            for (int x = 0; x < g; ++x) {
                const vec2 grid = seaFetchMapToGrid((glm::vec2(x, y) + 0.5f) / static_cast<float>(g), upwind);
                const glm::ivec2 at = glm::ivec2(glm::floor(grid * static_cast<float>(g)));
                const bool inside = at.x >= 0 && at.y >= 0 && at.x < g && at.y < g;
                const float fetch = inside ? radialFetch[at.y * g + at.x] : open;
                const float toEdge = inside ? (g - (at.x + 0.5f)) * texelYards : 0.0f;
                f.sums[y * g + x] += seaFetchRadialPart(fetch, toEdge, seaFetchRadialShare(radial), open,
                                                        0.5f * static_cast<float>(n));
            }
        }
    }
    return f;
}

/// The effective fetch for inland water by walking each radial upwind from
/// a point until the shore, a tenth of a yard at a time; a radial that runs
/// off what its grid holds counts for the least it can be.
float fetchByWalking(const Bay& bay, glm::ivec2 at, glm::vec2 toward) {
    using namespace wowee::rendering::glsl;
    const float n = static_cast<float>(bay.n);
    float sum = 0.0f;
    for (int radial = 0; radial < kSeaFetchRadials; ++radial) {
        const vec2 upwind = seaFetchRadialUpwind(-toward, radial);
        float fetch = -1.0f;
        float walked = 0.0f;
        for (; walked < 2.0f * n; walked += 0.1f) {
            const glm::vec2 q = glm::vec2(at) + upwind * walked;
            const vec2 g = seaFetchMapToGrid((q + 0.5f) / n, upwind);
            if (g.x < 0.0f || g.y < 0.0f || g.x >= 1.0f || g.y >= 1.0f) break;
            const glm::ivec2 p = glm::ivec2(glm::round(q));
            if (p.x < 0 || p.y < 0 || p.x >= bay.n || p.y >= bay.n) break;
            if (seaBedIsShore(bay.bed[p.y * bay.n + p.x], kRange)) {
                fetch = walked;
                break;
            }
        }
        const float share = seaFetchRadialShare(radial);
        sum += fetch >= 0.0f ? share * fetch : share * std::max(walked, 0.5f * n);
    }
    return sum;
}

/// A lake: shore all round a square `n` yards a side, `margin` in from the
/// map's edge.
Bay lake(int n, int margin) {
    Bay bay(n);
    for (int i = 0; i < n; ++i) {
        for (int m = 0; m < margin; ++m) {
            bay.shore({i, m});
            bay.shore({i, n - 1 - m});
            bay.shore({m, i});
            bay.shore({n - 1 - m, i});
        }
    }
    return bay;
}

}  // namespace

TEST_CASE("the open sea has the open sea's fetch, a lake past the map's edge at least the map's", "[water][sea_bed]") {
    // No shore anywhere in the map: the sea's water runs on to the open sea;
    // a lake's is no smaller than the map is wide from its middle, even at
    // the map's upwind edge, where it could have been taken for none.
    const Bay water(256);
    const EffectiveFetch fetch = fetchByPasses(water, glm::vec2(1.0f, 0.0f));
    for (glm::ivec2 p : {glm::ivec2(10, 10), glm::ivec2(128, 128), glm::ivec2(250, 40), glm::ivec2(4, 128)}) {
        CHECK(fetch.atMapTexel(p, true) > 1e5f);
        CHECK(fetch.atMapTexel(p, false) >= 0.85f * 128.0f);
    }
}

TEST_CASE("across a lake the fetch grows from the upwind shore to the downwind one", "[water][sea_bed]") {
    // A lake two hundred yards across, the wind along it and across it
    // aslant: the effective fetch, the Shore Protection Manual's, as the
    // radials' walk to the shore makes it.
    const int n = 256;
    const Bay bay = lake(n, 28);
    for (glm::vec2 toward : {glm::vec2(1.0f, 0.0f), glm::normalize(glm::vec2(0.6f, 0.8f))}) {
        const EffectiveFetch fetch = fetchByPasses(bay, toward);
        float last = -1.0f;
        for (float along = -80.0f; along <= 80.0f; along += 20.0f) {
            const glm::ivec2 p = glm::ivec2(glm::vec2(n / 2) + toward * along);
            const float found = fetch.atMapTexel(p);
            const float walked = fetchByWalking(bay, p, toward);
            // The grid's texel is four yards, and the shore is read as
            // whatever of it a texel touches.
            CHECK(found == Catch::Approx(walked).margin(10.0f).epsilon(0.15));
            CHECK(found > last);
            last = found;
        }
    }
}

TEST_CASE("a post stills no water behind it, a breakwater shelters what is behind it", "[water][sea_bed]") {
    // The wind from the north over the lake. A post in it, three yards
    // across: the waves come round it from either side. A breakwater sixty
    // yards long: close in its lee the water is sheltered.
    const int n = 256;
    const glm::vec2 fromNorth(0.0f, -1.0f);
    const Bay open = lake(n, 28);
    Bay withPost = open;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) withPost.shore({100 + dx, 150 + dy});
    }
    Bay withBreakwater = open;
    for (int x = 130; x < 190; ++x) {
        withBreakwater.shore({x, 150});
        withBreakwater.shore({x, 151});
    }
    const EffectiveFetch plain = fetchByPasses(open, fromNorth);
    const EffectiveFetch post = fetchByPasses(withPost, fromNorth);
    const EffectiveFetch breakwater = fetchByPasses(withBreakwater, fromNorth);
    // Just behind the post it holds back the radials it stands across, a
    // few of the fifteen; a little further on the waves from either side
    // have filled its lee in.
    CHECK(post.atMapTexel({100, 142}) > 0.6f * plain.atMapTexel({100, 142}));
    for (int below : {20, 40}) {
        const glm::ivec2 behindPost(100, 150 - below);
        CHECK(post.atMapTexel(behindPost) > 0.85f * plain.atMapTexel(behindPost));
    }
    const glm::ivec2 inLee(160, 140);
    CHECK(breakwater.atMapTexel(inLee) < 0.35f * plain.atMapTexel(inLee));
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

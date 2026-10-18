// Where the far ground is drawn from its bake (terrain_bake.hpp).
//
// A chunk is read from its 32x32 bake only where its layers would be read no
// finer: where a baked texel is no larger than a pixel on screen. What has to
// hold is which source a chunk gets for where it is - its layers near, its
// bake far, both across the band between - and that a slope, which reads its
// layers more finely than flat ground at the same distance, keeps them longer.

#include <catch_amalgamated.hpp>

#include "rendering/terrain_bake.hpp"

using wowee::rendering::TerrainBakeBand;
using wowee::rendering::terrainBakeBand;
using wowee::rendering::TerrainSurfaceSource;
using wowee::rendering::terrainSurfaceSourceFor;

namespace {

constexpr float kChunk = 1600.0f / 3.0f / 16.0f;  // a chunk's side, in yards
/// A 2560x1369 window's vertical focal length at a 60-degree field of view.
constexpr float kFocal = 1185.6f;
/// A chunk's half-diagonal, round its bounding sphere.
constexpr float kRadius = kChunk * 0.7072f;

TerrainSurfaceSource sourceAt(const TerrainBakeBand& band, float centre, float steepestUpZ = 1.0f) {
    return terrainSurfaceSourceFor(band, centre - kRadius, centre + kRadius, steepestUpZ);
}

}  // namespace

TEST_CASE("The bake takes over where one of its texels is a pixel", "[terrain_bake]") {
    const TerrainBakeBand band = terrainBakeBand(kChunk, kFocal);
    // A baked texel - the chunk over 32 - seen from the band's start spans
    // one pixel, and a quarter less across its end.
    CHECK(kChunk / 32.0f / band.start * kFocal == Catch::Approx(1.0f).epsilon(1e-4));
    CHECK(band.end > band.start);
    CHECK(kChunk / 32.0f / band.end * kFocal == Catch::Approx(0.8f).epsilon(1e-3));
}

TEST_CASE("Flat ground reads its layers near, its bake far, and both between", "[terrain_bake]") {
    const TerrainBakeBand band = terrainBakeBand(kChunk, kFocal);
    CHECK(sourceAt(band, 300.0f) == TerrainSurfaceSource::Layers);
    CHECK(sourceAt(band, band.start) == TerrainSurfaceSource::Crossfade);
    CHECK(sourceAt(band, band.end) == TerrainSurfaceSource::Crossfade);
    CHECK(sourceAt(band, band.end + kRadius + 1.0f) == TerrainSurfaceSource::Baked);
    CHECK(sourceAt(band, 2400.0f) == TerrainSurfaceSource::Baked);
}

TEST_CASE("A slope keeps its layers until it is as far as its steepness says", "[terrain_bake]") {
    const TerrainBakeBand band = terrainBakeBand(kChunk, kFocal);
    const float justPast = band.end + kRadius + 1.0f;
    // A cliff at half the slope's up-component reads its layers as finely as
    // flat ground at half the distance: past the band for flat ground, not
    // yet for it.
    CHECK(sourceAt(band, justPast, 1.0f) == TerrainSurfaceSource::Baked);
    CHECK(sourceAt(band, justPast, 0.5f) == TerrainSurfaceSource::Crossfade);
    CHECK(sourceAt(band, justPast * 2.2f, 0.5f) == TerrainSurfaceSource::Baked);
    // A wall never takes the bake whole.
    CHECK(sourceAt(band, 100000.0f, 0.0f) == TerrainSurfaceSource::Crossfade);
}

TEST_CASE("Zooming in pushes the bake further out", "[terrain_bake]") {
    // A narrower field of view is a longer focal length: the same chunk covers
    // more pixels, so its layers are read finely further away.
    const TerrainBakeBand wide = terrainBakeBand(kChunk, kFocal);
    const TerrainBakeBand narrow = terrainBakeBand(kChunk, kFocal * 3.0f);
    const float distance = wide.end + kRadius + 1.0f;
    CHECK(sourceAt(wide, distance) == TerrainSurfaceSource::Baked);
    CHECK(sourceAt(narrow, distance) != TerrainSurfaceSource::Baked);
}

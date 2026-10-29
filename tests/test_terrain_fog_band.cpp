// Which terrain chunks draw with the shader that cannot discard: only those
// no part of which reaches the fog band, where the ground is cut away for the
// sky, or the hand-over to the distant terrain, where it is dithered away. A
// chunk that reaches either drawn without the discards shows ground there
// that should be gone.
#include <catch_amalgamated.hpp>
#include "rendering/terrain_fog_band.hpp"

using wowee::rendering::terrainChunkShortOfDiscards;

// A chunk is 33.3 yards on a side; its bounding sphere is about 24 across the
// diagonal before relief. The fog band is the view distance, 1900 yards here;
// the hand-over to the distant terrain starts at 3000.

TEST_CASE("A chunk well inside both distances needs no discard", "[terrain][fog]") {
    REQUIRE(terrainChunkShortOfDiscards(500.0f, 24.0f, 1900.0f, 3000.0f));
}

TEST_CASE("A chunk whose centre is inside but whose edge crosses the fog band keeps the discard",
          "[terrain][fog]") {
    // Centre 1890 yards out, 10 short of the band; its far side is 1914 out.
    REQUIRE_FALSE(terrainChunkShortOfDiscards(1890.0f, 24.0f, 1900.0f, 3000.0f));
}

TEST_CASE("A chunk ending just short of the fog band keeps the discard", "[terrain][fog]") {
    // Far side at 1899.5: half a yard is inside the rounding allowance.
    REQUIRE_FALSE(terrainChunkShortOfDiscards(1875.5f, 24.0f, 1900.0f, 3000.0f));
}

TEST_CASE("A chunk ending two yards short of the fog band needs no discard", "[terrain][fog]") {
    REQUIRE(terrainChunkShortOfDiscards(1874.0f, 24.0f, 1900.0f, 3000.0f));
}

TEST_CASE("A chunk reaching the distant-terrain hand-over keeps the discard", "[terrain][fog]") {
    // View distance 5000, so the fog band is far away; the hand-over at 3000
    // starts dithering this chunk's far side.
    REQUIRE_FALSE(terrainChunkShortOfDiscards(2990.0f, 24.0f, 5000.0f, 3000.0f));
}

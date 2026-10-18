// Which WMO group batches draw as one.
//
// A merged batch is drawn with the material state of the first batch in it,
// so two batches may merge only when that state is the same for both. The
// merge used to key on the texture, alpha test, unlit, glass and emissive
// only: an opaque and a blended material on one texture came out both one
// way, and the two banners of a Stormwind gate, one texture, swung about the
// first banner's pivot.
#include <catch_amalgamated.hpp>

#include "rendering/wmo_batch_merge.hpp"

#include <vector>

using wowee::rendering::groupWmoBatches;
using wowee::rendering::WmoBatchState;
using wowee::rendering::WmoLight;

namespace {

const int kStone = 0;
const int kBanner = 1;

using Groups = std::vector<std::vector<size_t>>;

}  // namespace

TEST_CASE("batches with the same state draw as one", "[wmo-batch-merge]") {
    const WmoBatchState wall{ .texture = &kStone };
    // The merge is what the batching is for: a wall in three batches binds
    // its material once.
    CHECK(groupWmoBatches({ wall, wall, wall }) == Groups{ { 0, 1, 2 } });
}

TEST_CASE("merged batches keep the order their batches came in",
          "[wmo-batch-merge]") {
    const WmoBatchState wall{ .texture = &kStone };
    const WmoBatchState banner{ .texture = &kBanner };
    CHECK(groupWmoBatches({ banner, wall, banner, wall }) ==
          Groups{ { 0, 2 }, { 1, 3 } });
    CHECK(groupWmoBatches({}) == Groups{});
}

TEST_CASE("an opaque and a blended material on one texture stay apart",
          "[wmo-batch-merge]") {
    // The reported case: blend 0 and blend 2 on one texture became a single
    // batch, drawn all opaque or all transparent by whichever came first.
    const WmoBatchState opaque{ .texture = &kStone, .blendMode = 0 };
    const WmoBatchState blended{ .texture = &kStone, .blendMode = 2 };
    CHECK(groupWmoBatches({ opaque, blended }) == Groups{ { 0 }, { 1 } });

    // Blend modes past 2 are their own materials too, and so is alpha test.
    const WmoBatchState additive{ .texture = &kStone, .blendMode = 3 };
    const WmoBatchState alphaTested{ .texture = &kStone, .blendMode = 1 };
    CHECK(groupWmoBatches({ blended, additive, alphaTested, opaque }) ==
          Groups{ { 0 }, { 1 }, { 2 }, { 3 } });
}

TEST_CASE("two banners on one texture keep their own pivots",
          "[wmo-batch-merge]") {
    // The two banners of a gate: one texture, hung at the same height, either
    // side of the arch.
    const WmoBatchState left{ .texture = &kBanner, .clothTop = 120.0f,
                              .clothDrop = 18.0f, .clothCentreX = -9.0f,
                              .clothCentreY = 4.0f };
    WmoBatchState right = left;
    right.clothCentreX = 9.0f;
    CHECK(groupWmoBatches({ left, right }) == Groups{ { 0 }, { 1 } });

    // A banner and a wall on the same texture: the wall must not sway.
    const WmoBatchState wall{ .texture = &kBanner };
    CHECK(groupWmoBatches({ left, wall }) == Groups{ { 0 }, { 1 } });
}

TEST_CASE("lava does not merge with rock on the same texture",
          "[wmo-batch-merge]") {
    // A lava batch scrolls its texture and lights the room around it; the
    // rock beside it does neither.
    const WmoBatchState rock{ .texture = &kStone };
    const WmoBatchState lava{ .texture = &kStone, .isLava = true };
    CHECK(groupWmoBatches({ rock, lava }) == Groups{ { 0 }, { 1 } });
}

TEST_CASE("the shading state that merged before still splits",
          "[wmo-batch-merge]") {
    const WmoBatchState plain{ .texture = &kStone };
    const WmoBatchState unlit{ .texture = &kStone,
                               .lighting = { .light = WmoLight::VertexColour } };
    const WmoBatchState glass{ .texture = &kStone, .sidnColor = 0xffffffffu };
    const WmoBatchState lamp{ .texture = &kStone, .emissiveLevel = 1 };
    const WmoBatchState otherTexture{ .texture = &kBanner };
    CHECK(groupWmoBatches({ plain, unlit, glass, lamp, otherTexture }) ==
          Groups{ { 0 }, { 1 }, { 2 }, { 3 }, { 4 } });
}

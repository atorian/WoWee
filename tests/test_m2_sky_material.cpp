#include <catch_amalgamated.hpp>
#include "test_support.hpp"
#include "rendering/m2_sky_material.hpp"

using namespace wowee::rendering;
using wowee::pipeline::M2Loader;
using wowee::pipeline::M2AnimationTrack;

TEST_CASE("sky day clock wraps and uses the authored sequence duration", "[m2][sky]") {
    CHECK(skyAnimationTimeMs(15.0f, 320000) == Catch::Approx(200000));
    CHECK(skyAnimationTimeMs(39.0f, 320000) == Catch::Approx(200000));
    CHECK(skyAnimationTimeMs(-9.0f, 320000) == Catch::Approx(200000));
    CHECK(skyAnimationTimeMs(24.0f, 320000) == 0.0f);
}

TEST_CASE("cloud global fades retain their elapsed clock and zero opacity", "[m2][sky]") {
    M2AnimationTrack track;
    track.interpolationType = 1;
    track.globalSequence = 0;
    track.sequences.resize(1);
    track.sequences[0].timestamps = {0, 500, 1000};
    track.sequences[0].floatValues = {0, 1, 0};
    const std::vector<M2AnimationTrack> colors{track};
    CHECK(skyBatchOpacity(colors, 0, {}, UINT16_MAX, 200000, 0, {1000}) == 0.0f);
    CHECK(skyBatchOpacity(colors, 0, {}, UINT16_MAX, 200000, 250, {1000}) == Catch::Approx(0.5f));
    CHECK(skyBatchOpacity(colors, 0, {}, UINT16_MAX, 0, 1250, {1000}) == Catch::Approx(0.5f));
    CHECK(skyBatchOpacity({}, UINT16_MAX, {}, UINT16_MAX, 200000, 0, {}) == 1.0f);
}

TEST_CASE("Dalaran sky uses day clouds and masks all 21 aurora layers", "[m2][sky][asset]") {
    const std::string root = "Data/expansions/wotlk/environments/stars/";
    const auto data = wowee::test::readFile(root + "dalaranskybox.m2");
    if (data.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    auto model = M2Loader::load(data);
    const auto skin = wowee::test::readFile(root + "dalaranskybox00.skin");
    REQUIRE_FALSE(skin.empty());
    REQUIRE(M2Loader::loadSkin(skin, model));
    REQUIRE(model.sequences.size() == 1);
    REQUIRE(model.sequences[0].duration == 320000);
    REQUIRE(model.textureCoordLookup == std::vector<int16_t>{0, 1});
    REQUIRE(model.textureWeightLookup.size() == 7);
    const auto opacity = [&](uint16_t weight, float hours) {
        return skyBatchOpacity({}, UINT16_MAX, model.textureWeightTracks,
            model.textureWeightLookup[weight], skyAnimationTimeMs(hours, 320000),
            5000, model.globalSequenceDurations);
    };
    CHECK(opacity(2, 15) == Catch::Approx(1)); // day clouds
    CHECK(opacity(3, 15) == 0); // sunset clouds must not cover the day
    CHECK(opacity(4, 15) == 0); // night clouds must not cover the day
    CHECK(opacity(2, 3) == 0);
    CHECK(opacity(4, 3) == Catch::Approx(1));
    CHECK(skyBatchOpacity(model.colorAlphaTracks, 0, {}, UINT16_MAX,
        skyAnimationTimeMs(15, 320000), 5000, model.globalSequenceDurations) == 0); // day stars
    CHECK(skyBatchOpacity(model.colorAlphaTracks, 0, {}, UINT16_MAX,
        skyAnimationTimeMs(3, 320000), 5000, model.globalSequenceDurations) == Catch::Approx(1));
    size_t masks = 0;
    for (const auto& batch : model.batches) {
        if (batch.textureCount != 2) continue;
        REQUIRE(batch.shader == 0);
        REQUIRE(model.materials[batch.materialIndex].blendMode == 4);
        REQUIRE(batch.textureIndex + 1 < model.textureLookup.size());
        const auto texture = model.textureLookup[batch.textureIndex + 1];
        REQUIRE(texture < model.textures.size());
        CHECK(model.textures[texture].filename.find("AURORAMASK_B_ORANGE") != std::string::npos);
        CHECK(model.textureCoordLookup[batch.textureUnit + 1] == 1);
        ++masks;
    }
    CHECK(masks == 21);
}

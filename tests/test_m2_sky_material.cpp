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

TEST_CASE("a batch's alpha is fixed when its tracks never change and sampled when they do",
          "[m2][opacity]") {
    M2AnimationTrack fixed;
    fixed.sequences.resize(2);
    fixed.sequences[0].timestamps = {0};
    fixed.sequences[0].floatValues = {0.5f};
    fixed.sequences[1].timestamps = {0};
    fixed.sequences[1].floatValues = {0.5f};
    M2AnimationTrack hidden;
    hidden.sequences.resize(1);
    hidden.sequences[0].timestamps = {0};
    hidden.sequences[0].floatValues = {0.0f};
    M2AnimationTrack fading;
    fading.interpolationType = 1;
    fading.sequences.resize(1);
    fading.sequences[0].timestamps = {0, 1000};
    fading.sequences[0].floatValues = {0.0f, 1.0f};
    const std::vector<M2AnimationTrack> colors{fixed, hidden, fading};

    const auto still = m2BatchOpacity(colors, 0, {}, UINT16_MAX);
    CHECK_FALSE(still.animated);
    CHECK(still.atRest == Catch::Approx(0.5f));
    // A layer whose alpha is zero for its whole life is not drawn - it used
    // to be read as "not set" and drawn solid.
    const auto gone = m2BatchOpacity(colors, 1, {}, UINT16_MAX);
    CHECK_FALSE(gone.animated);
    CHECK(gone.atRest <= kM2BatchHiddenOpacity);
    const auto moving = m2BatchOpacity(colors, 2, {}, UINT16_MAX);
    CHECK(moving.animated);
    CHECK(moving.atRest == 1.0f);
    CHECK(m2SampleBatchOpacity(colors, 2, {}, UINT16_MAX, 0, 0.0f, 0.0f, {}) <= kM2BatchHiddenOpacity);
    CHECK(m2SampleBatchOpacity(colors, 2, {}, UINT16_MAX, 0, 500.0f, 0.0f, {}) == Catch::Approx(0.5f));
    // No tracks at all: the layer is fully there.
    const auto none = m2BatchOpacity({}, UINT16_MAX, {}, UINT16_MAX);
    CHECK_FALSE(none.animated);
    CHECK(none.atRest == 1.0f);
}

TEST_CASE("a sequence with no alpha keys reads as fully there", "[m2][opacity]") {
    // Dalaran's fountain: the basin's alpha has no keys in Stand and Closed
    // and 0 in Opened; the statue's is 0 in Stand and Closed and has no keys
    // in Opened. Each shows in the sequences without keys.
    M2AnimationTrack basin;
    basin.sequences.resize(3);
    basin.sequences[2].timestamps = {0};
    basin.sequences[2].floatValues = {0.0f};
    M2AnimationTrack statue;
    statue.sequences.resize(3);
    statue.sequences[0].timestamps = {0};
    statue.sequences[0].floatValues = {0.0f};
    statue.sequences[1].timestamps = {0};
    statue.sequences[1].floatValues = {0.0f};
    const std::vector<M2AnimationTrack> colors{basin, statue};

    CHECK(m2BatchOpacity(colors, 0, {}, UINT16_MAX).animated);
    CHECK(m2BatchOpacity(colors, 1, {}, UINT16_MAX).animated);
    CHECK(m2SampleBatchOpacity(colors, 0, {}, UINT16_MAX, 0, 0.0f, 0.0f, {}) == 1.0f);
    CHECK(m2SampleBatchOpacity(colors, 0, {}, UINT16_MAX, 2, 0.0f, 0.0f, {}) <= kM2BatchHiddenOpacity);
    CHECK(m2SampleBatchOpacity(colors, 1, {}, UINT16_MAX, 0, 0.0f, 0.0f, {}) <= kM2BatchHiddenOpacity);
    CHECK(m2SampleBatchOpacity(colors, 1, {}, UINT16_MAX, 2, 0.0f, 0.0f, {}) == 1.0f);
}

TEST_CASE("a tournament pennant's mesh is there by its own data", "[m2][opacity][asset]") {
    // The pennant an Argent Tournament champion wears is a spell effect with a
    // particle emitter and two batches; it was dropped whole as "emitter-only
    // geometry" and only its spark was drawn.
    const std::string root = "Data/expansions/wotlk/world/expansion02/doodads/generic/argentcrusade/pennants/";
    const auto data = wowee::test::readFile(root + "tournament_pennant_human03.m2");
    if (data.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    auto model = M2Loader::load(data);
    const auto skin = wowee::test::readFile(root + "tournament_pennant_human0300.skin");
    REQUIRE_FALSE(skin.empty());
    REQUIRE(M2Loader::loadSkin(skin, model));
    REQUIRE_FALSE(model.particleEmitters.empty());
    REQUIRE_FALSE(model.batches.empty());
    for (const auto& batch : model.batches) {
        const uint16_t weight = batch.transparencyIndex < model.textureWeightLookup.size()
            ? model.textureWeightLookup[batch.transparencyIndex] : UINT16_MAX;
        const auto opacity = m2BatchOpacity(model.colorAlphaTracks, batch.colorIndex,
                                            model.textureWeightTracks, weight);
        CHECK_FALSE(opacity.animated);
        CHECK(opacity.atRest >= kM2BatchOpaqueOpacity);
    }
}

TEST_CASE("Icy Touch's fading layers are sampled, not frozen at load", "[m2][opacity][asset]") {
    const std::string root = "Data/expansions/wotlk/spells/";
    const auto data = wowee::test::readFile(root + "deathknight_icytouch.m2");
    if (data.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    auto model = M2Loader::load(data);
    const auto skin = wowee::test::readFile(root + "deathknight_icytouch00.skin");
    REQUIRE_FALSE(skin.empty());
    REQUIRE(M2Loader::loadSkin(skin, model));
    size_t animated = 0;
    for (const auto& batch : model.batches) {
        const uint16_t weight = batch.transparencyIndex < model.textureWeightLookup.size()
            ? model.textureWeightLookup[batch.transparencyIndex] : UINT16_MAX;
        if (m2BatchOpacity(model.colorAlphaTracks, batch.colorIndex,
                           model.textureWeightTracks, weight).animated) ++animated;
    }
    CHECK(animated > 0);
}

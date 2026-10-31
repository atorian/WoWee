// M2 struct layout and field tests (header-only, no loader source)
#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include "pipeline/m2_model_classifier.hpp"
#include "rendering/m2_renderer_internal.h"
#include <cstring>

using namespace wowee::pipeline;

TEST_CASE("Animated glow centers follow their skinned bones", "[m2][lights]") {
    wowee::rendering::M2Instance instance;
    instance.modelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    instance.boneMatrices = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 2.0f))
    };

    wowee::rendering::M2ModelGPU::BatchGPU batch;
    // Average of two rigid vertices: (0,0,0) on bone 0 and (2,0,0)
    // on bone 1. Moving bone 1 upward by two moves their center upward by one.
    batch.lightBoneAnchors.push_back({0, glm::vec4(0.0f, 0.0f, 0.0f, 0.5f)});
    batch.lightBoneAnchors.push_back({1, glm::vec4(1.0f, 0.0f, 0.0f, 0.5f)});

    const glm::vec3 center = wowee::rendering::animatedBatchWorldCenter(instance, batch);
    CHECK(center.x == Catch::Approx(11.0f));
    CHECK(center.y == Catch::Approx(0.0f));
    CHECK(center.z == Catch::Approx(1.0f));
}

TEST_CASE("Hanging lantern light pools project from suspension to animated tip", "[m2][lights]") {
    wowee::rendering::M2Instance instance;
    instance.modelMatrix = glm::mat4(1.0f);
    instance.scale = 1.0f;
    instance.boneMatrices = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f))
    };

    wowee::rendering::M2ModelGPU::BatchGPU batch;
    batch.lightBoneAnchors.push_back({1, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)});
    batch.lightSuspensionBone = 0;
    batch.lightSuspensionPoint = glm::vec3(0.0f, 0.0f, 2.0f);

    CHECK(wowee::rendering::animatedBatchWorldCenter(instance, batch).x ==
          Catch::Approx(1.0f));
    const glm::vec3 pool = wowee::rendering::animatedBatchLightWorldCenter(instance, batch);
    CHECK(pool.x == Catch::Approx(4.0f));
    CHECK(pool.y == Catch::Approx(0.0f));
    CHECK(pool.z == Catch::Approx(1.0f));
}

TEST_CASE("Lighthouse beams are classified for distant bone updates", "[m2][animation]") {
    const auto beam = wowee::pipeline::classifyM2Model(
        "World\\Generic\\Human\\Passive Doodads\\Stormwind\\Stormwind_LighthouseBeam_01.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 100, 0);
    REQUIRE(beam.isLightBeam);
    REQUIRE_FALSE(beam.disableAnimation);
}

TEST_CASE("Ship machinery is classified for distant bone updates", "[m2][animation][transport]") {
    const auto sails = wowee::pipeline::classifyM2Model(
        "World\\Generic\\PassiveDoodads\\Ships\\ShipAnimation\\TransportShip_Sails.m2",
        glm::vec3(-40.0f), glm::vec3(40.0f), 161, 0);
    const auto paddle = wowee::pipeline::classifyM2Model(
        "World\\Generic\\PassiveDoodads\\Ships\\ShipAnimation\\PaddleWheel\\Icebreaker_Paddlewheel.m2",
        glm::vec3(-14.0f), glm::vec3(14.0f), 2676, 4);
    REQUIRE(sails.isTransportDoodad);
    REQUIRE(paddle.isTransportDoodad);
}

TEST_CASE("Steam Tonk vehicles are not steam VFX", "[m2][classification]") {
    // The TBC SteamTonk overlay is a tiny proxy that slips under the
    // low-poly vertex gate; combined with a smoke emitter it was mis-classified
    // as an additive spell effect (glowing translucent). A "tonk"/"tank" name
    // must never be treated as steam VFX regardless of geometry.
    const auto tonkLow = wowee::pipeline::classifyM2Model(
        "Creature\\SteamTonk\\SteamTonk.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 23, 2);
    REQUIRE_FALSE(tonkLow.isSpellEffect);
    const auto tonkHi = wowee::pipeline::classifyM2Model(
        "Creature\\SteamTonk\\SteamTonk.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 606, 1);
    REQUIRE_FALSE(tonkHi.isSpellEffect);

    // A genuine low-poly steam emitter is still a spell effect.
    const auto steam = wowee::pipeline::classifyM2Model(
        "Spells\\Steam.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 40, 2);
    REQUIRE(steam.isSpellEffect);
}

TEST_CASE("M2AnimationTrack hasData", "[m2]") {
    M2AnimationTrack track;
    REQUIRE_FALSE(track.hasData());

    track.sequences.push_back({});
    REQUIRE(track.hasData());
}

TEST_CASE("M2Model isValid", "[m2]") {
    M2Model model{};
    REQUIRE_FALSE(model.isValid()); // no vertices or indices

    model.vertices.push_back({});
    REQUIRE_FALSE(model.isValid()); // vertices but no indices

    model.indices.push_back(0);
    REQUIRE(model.isValid()); // both present
}


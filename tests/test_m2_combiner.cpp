// How an M2 batch combines its textures, against the 3.3.5a client's rules
// (WoW.exe 12340, 0x836980 and 0x836600; render spec 6.1).
#include <catch_amalgamated.hpp>

#include "rendering/m2_combiner.hpp"

using namespace wowee::rendering;

TEST_CASE("Without flag 0x08 the file's shader id is ignored", "[m2][combiner]") {
    // Dalaran's lamp: an additive reflection layer, one texture, sphere map.
    const auto reflection = m2BatchShading(0x0, {}, 0, 1, 4, -1, 0);
    CHECK(reflection.combiner == M2Combiner::Mod);
    CHECK(reflection.coords0 == M2StageCoords::Environment);
    // Its opaque glass: Opaque, whatever the shader id says.
    CHECK(m2BatchShading(0x0, {}, 7, 1, 0, 0, 0).combiner == M2Combiner::Opaque);
    // A one-texture batch on the second UV set.
    CHECK(m2BatchShading(0x0, {}, 0, 1, 2, 1, 0).coords0 == M2StageCoords::Uv1);
}

TEST_CASE("With flag 0x08 the combo table names the stages", "[m2][combiner]") {
    // The Vrykul chandelier: combos [1, 4] - Mod, then Mod2x.
    const std::vector<uint16_t> combos{1, 4};
    const auto flame = m2BatchShading(0x18, combos, 0, 2, 4, 0, 1);
    CHECK(flame.combiner == M2Combiner::ModMod2x);
    CHECK(flame.stages == 2);
    CHECK(flame.coords0 == M2StageCoords::Uv0);
    CHECK(flame.coords1 == M2StageCoords::Uv1);
    // An opaque material forces the first stage to Opaque.
    CHECK(m2BatchShading(0x18, combos, 0, 2, 0, 0, 1).combiner == M2Combiner::OpaqueMod2x);
}

TEST_CASE("Armour shine: an opaque layer doubled by a sphere-mapped reflection", "[m2][combiner]") {
    // Opaque then Mod2xNA, the second stage on the environment map.
    const std::vector<uint16_t> combos{0, 6};
    const auto armour = m2BatchShading(0x08, combos, 0, 2, 0, 0, -1);
    CHECK(armour.combiner == M2Combiner::OpaqueMod2xNA);
    CHECK(armour.coords1 == M2StageCoords::Environment);
    CHECK(m2CombinerUsesSecondStage(armour.combiner));
}

TEST_CASE("A sphere-mapped first stage samples its second with UV0, unanimated", "[m2][combiner]") {
    const std::vector<uint16_t> combos{1, 1};
    const auto s = m2BatchShading(0x08, combos, 0, 2, 2, -1, 0);
    CHECK(s.coords0 == M2StageCoords::Environment);
    CHECK(s.coords1 == M2StageCoords::Uv0Still);
}

TEST_CASE("A pair the client has no shader for is drawn Mod_Mod", "[m2][combiner]") {
    // Add then Add: no shader reaches it.
    const std::vector<uint16_t> combos{3, 3};
    CHECK(m2BatchShading(0x08, combos, 0, 2, 2, 0, 1).combiner == M2Combiner::ModMod);
}

TEST_CASE("The fixed shaders of the 0x8000 range", "[m2][combiner]") {
    CHECK_FALSE(m2BatchShading(0x08, {}, 0x8000, 1, 0, 0, 0).drawn);
    const auto alpha = m2BatchShading(0x08, {}, 0x8001, 1, 0, 0, 0);
    CHECK(alpha.combiner == M2Combiner::OpaqueMod2xNAAlpha);
    CHECK(alpha.stages == 2);
    CHECK(alpha.coords1 == M2StageCoords::Environment);
}

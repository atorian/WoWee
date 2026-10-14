// Whether an M2 batch is alpha tested, which decides the pipeline it draws on.
//
// A batch whose texture carries no alpha channel was forced onto the cutout
// pipeline whenever its blend mode was 2 or higher. For alpha blend that is
// reasonable - blending by an alpha that is 1 everywhere draws solid, so
// cutting out at least keeps the artist's silhouette.
//
// For the additive modes it destroys the effect. Additive does not use alpha
// for transparency: black is what disappears, because adding zero changes
// nothing. Glow cards are authored exactly that way - an opaque texture,
// bright in the middle and black at the edges.
//
// The oracle is the model and the texture Orgrimmar's bonfire ships.
// orcpvpbonfirelarge material 0 is blend mode 4, and its texture
// GENERICGLOW_ALPHA_128.BLP reports alphaDepth 0 in its own header - named for
// an alpha it does not have. Forced to cutout, every texel passed the test and
// the flame rendered as a flat opaque grey disc behind the logs.
#include <catch_amalgamated.hpp>

#include "rendering/m2_blend_mode.hpp"

using wowee::rendering::m2BatchNeedsAlphaTest;
using wowee::rendering::m2BlendIsAdditive;

TEST_CASE("the Orgrimmar bonfire's glow card is not alpha tested", "[m2]") {
    // Blend mode 4, no alpha channel: the case that showed it.
    CHECK_FALSE(m2BatchNeedsAlphaTest(4, false));
    // And it stays untested if the texture does happen to carry alpha.
    CHECK_FALSE(m2BatchNeedsAlphaTest(4, true));
}

TEST_CASE("no additive batch is alpha tested", "[m2]") {
    // Black is the transparent colour for these, whatever the texture holds.
    for (uint8_t mode : {uint8_t{3}, uint8_t{4}}) {
        INFO("blend mode " << int(mode));
        CHECK(m2BlendIsAdditive(mode));
        CHECK_FALSE(m2BatchNeedsAlphaTest(mode, false));
        CHECK_FALSE(m2BatchNeedsAlphaTest(mode, true));
    }
}

TEST_CASE("alpha key is tested where there is alpha to key on", "[m2]") {
    CHECK(m2BatchNeedsAlphaTest(1, true));
    // And not where there is none. Every texel passes a test against an alpha
    // that is 1 everywhere, so the test decides nothing - but it still puts
    // the batch on the cutout pipeline and its alpha-to-coverage. Tirisfal's
    // canopy trunks are drawn alpha-keyed over a DXT1 texture with no
    // punch-through block in it; the one of the seven whose trunk is plain
    // opaque is the one that renders right.
    CHECK_FALSE(m2BatchNeedsAlphaTest(1, false));
}

TEST_CASE("a blended batch with no alpha still falls back to cutout",
          "[m2]") {
    // The reason the rule existed. Blending by an alpha that is 1 everywhere
    // draws the quad solid, so testing gives the silhouette a chance.
    CHECK(m2BatchNeedsAlphaTest(2, false));
    // With a real alpha channel it blends, as authored.
    CHECK_FALSE(m2BatchNeedsAlphaTest(2, true));
}

TEST_CASE("opaque is never tested", "[m2]") {
    CHECK_FALSE(m2BatchNeedsAlphaTest(0, true));
    CHECK_FALSE(m2BatchNeedsAlphaTest(0, false));
    CHECK_FALSE(m2BlendIsAdditive(0));
    CHECK_FALSE(m2BlendIsAdditive(1));
    CHECK_FALSE(m2BlendIsAdditive(2));
}

TEST_CASE("modulate layers multiply RGB independently of atlas alpha", "[m2]") {
    CHECK_FALSE(m2BatchNeedsAlphaTest(5, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(6, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(5, true));
    CHECK_FALSE(m2BatchNeedsAlphaTest(6, true));
}


TEST_CASE("opaque mount skin does not inherit atlas transparency", "[m2][mount]") {
    using wowee::rendering::characterBatchNeedsAlphaTest;
    CHECK_FALSE(characterBatchNeedsAlphaTest(0, true, false, false));
    CHECK_FALSE(characterBatchNeedsAlphaTest(0, true, false, true));
    CHECK(characterBatchNeedsAlphaTest(1, true, false, false));
    CHECK(characterBatchNeedsAlphaTest(0, true, true, false));
    CHECK_FALSE(characterBatchNeedsAlphaTest(2, true, false, false));
    for (uint16_t mode : {3, 4, 5, 6, 7}) {
        INFO("blend mode " << mode);
        CHECK_FALSE(characterBatchNeedsAlphaTest(mode, false, false, false));
    }
}

using wowee::rendering::CharacterBatchBlendInputs;
using wowee::rendering::CharacterBlendPipeline;
using wowee::rendering::characterBatchUnlit;
using wowee::rendering::characterBlendPipeline;

TEST_CASE("a character's Mod and Mod2x layers multiply what is behind them",
          "[m2][character]") {
    // Mod2x was drawn additive and Mod with the alpha blend.
    CHECK(characterBlendPipeline({.blendMode = 5}) == CharacterBlendPipeline::Modulate);
    CHECK(characterBlendPipeline({.blendMode = 6}) == CharacterBlendPipeline::Modulate2x);
}

TEST_CASE("a fading character keeps its additive and Mod layers on their own blend",
          "[m2][character]") {
    // The fade is applied in the shader. On the alpha blend an additive card's
    // black backing and a Mod layer's whole texture would be painted on.
    CHECK(characterBlendPipeline({.blendMode = 4, .opacity = 0.5f}) ==
          CharacterBlendPipeline::Additive);
    CHECK(characterBlendPipeline({.blendMode = 5, .opacity = 0.5f}) ==
          CharacterBlendPipeline::Modulate);
    CHECK(characterBlendPipeline({.blendMode = 6, .opacity = 0.5f}) ==
          CharacterBlendPipeline::Modulate2x);
}

TEST_CASE("a fading character's solid parts keep writing depth, its cards do not",
          "[m2][character]") {
    CHECK(characterBlendPipeline({.blendMode = 0, .opacity = 0.5f}) ==
          CharacterBlendPipeline::Translucent);
    // The glue screens' haze cards: opaque material, no-depth-write flag.
    CHECK(characterBlendPipeline({.blendMode = 0, .materialFlags = 0x10, .opacity = 0.5f}) ==
          CharacterBlendPipeline::Alpha);
}

TEST_CASE("an enchant's layers draw as their materials say", "[m2][character]") {
    // Rockbiter's stones are opaque; its glow cards are blend mode 4.
    CHECK(characterBlendPipeline({.blendMode = 0}) == CharacterBlendPipeline::Opaque);
    CHECK(characterBlendPipeline({.blendMode = 4}) == CharacterBlendPipeline::Additive);
    CHECK(characterBlendPipeline({.blendMode = 5}) == CharacterBlendPipeline::Modulate);
}

TEST_CASE("a character's Mod layers are lit unless their material says unlit",
          "[m2][character]") {
    CHECK_FALSE(characterBatchUnlit(5, 0));
    CHECK_FALSE(characterBatchUnlit(6, 0));
    CHECK(characterBatchUnlit(5, 0x01));
    // Additive layers emit their own light.
    CHECK(characterBatchUnlit(4, 0));
    CHECK_FALSE(characterBatchUnlit(0, 0));
}

using wowee::rendering::M2BatchMaterialInputs;
using wowee::rendering::M2BlendPipeline;
using wowee::rendering::m2BatchDrawState;
using wowee::rendering::m2BlendPipeline;
TEST_CASE("a spell effect's layers draw as their materials say", "[m2]") {
    // Battle Stance's mace is opaque and the arcane missile's core too; the
    // client does not add them like glow cards. Mod still darkens.
    for (uint8_t mode = 0; mode <= 6; ++mode) {
        INFO("blend mode " << int(mode));
        M2BatchMaterialInputs in;
        in.blendMode = mode;
        in.hasAlpha = true;
        in.spellEffect = true;
        const auto s = m2BatchDrawState(in);
        CHECK(s.drawBlendMode == mode);
    }
    M2BatchMaterialInputs opaque;
    opaque.spellEffect = true;
    CHECK(m2BlendPipeline(m2BatchDrawState(opaque).drawBlendMode) == M2BlendPipeline::Opaque);
}

TEST_CASE("NoAlphaAdd does not share Add's alpha-scaled pipeline", "[m2]") {
    // Mode 3 adds the whole texel whatever its alpha says; a mode 3 texture
    // with an empty alpha channel vanished on the SRC_ALPHA pipeline.
    CHECK(m2BlendPipeline(3) == M2BlendPipeline::NoAlphaAdd);
    CHECK(m2BlendPipeline(4) == M2BlendPipeline::AlphaAdd);
}

TEST_CASE("an opaque batch of a fire is drawn opaque", "[m2]") {
    // A campfire's rocks and a forge's coals: an opaque material, a texture
    // with no alpha. The client draws them solid; nothing keys their black.
    M2BatchMaterialInputs in;
    in.blendMode = 0;
    const auto s = m2BatchDrawState(in);
    CHECK_FALSE(s.forceCutout);
    CHECK(s.alphaTest == 0);
    CHECK(m2BlendPipeline(s.drawBlendMode) == M2BlendPipeline::Opaque);
}

TEST_CASE("an opaque foliage batch is cut out only where its alpha is a silhouette", "[m2]") {
    // Silverpine's trunk: opaque, sampling an atlas panel with a stale hole.
    M2BatchMaterialInputs trunk;
    trunk.blendMode = 0;
    trunk.hasAlpha = true;
    trunk.foliageLike = true;
    CHECK_FALSE(m2BatchDrawState(trunk).forceCutout);

    M2BatchMaterialInputs leaves = trunk;
    leaves.alphaIsSilhouette = true;
    const auto s = m2BatchDrawState(leaves);
    CHECK(s.forceCutout);
    CHECK(s.alphaTest == 2);
}

TEST_CASE("a keyed material is cut at the client's own alpha key", "[m2]") {
    // dalaran_inn_window.m2: blend mode 1 over DAL_WINDOWCUTOUT.
    M2BatchMaterialInputs window;
    window.blendMode = 1;
    window.hasAlpha = true;
    const auto s = m2BatchDrawState(window);
    CHECK(s.forceCutout);
    CHECK(s.alphaTest == 4);

    // Keyed leaves keep the foliage cutoff.
    M2BatchMaterialInputs leaves = window;
    leaves.foliageLike = true;
    CHECK(m2BatchDrawState(leaves).alphaTest == 2);
}

TEST_CASE("ground clutter is cut out and lit", "[m2]") {
    M2BatchMaterialInputs in;
    in.blendMode = 0;
    in.groundDetail = true;
    const auto s = m2BatchDrawState(in);
    CHECK(s.forceCutout);
    CHECK(s.alphaTest == 3);
    CHECK(s.litOverride);
}

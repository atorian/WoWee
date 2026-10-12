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
using wowee::rendering::m2BatchWantsColorKey;
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

TEST_CASE("an additive card is not colour keyed", "[m2]") {
    // The key discards every texel below a threshold so a card with a black
    // backing can be drawn opaquely. Additive has no such problem, and the key
    // ruins it: the transparent pass raised the threshold to 0.7 for blend
    // mode 4, and a glow card is a radial gradient from black to white, so
    // everything below the bright core was thrown away and the soft falloff
    // became a hard-edged disc. That is what Orgrimmar's bonfires were.
    CHECK_FALSE(m2BatchWantsColorKey(4, true));
    CHECK_FALSE(m2BatchWantsColorKey(3, true));
}

TEST_CASE("opaque and alpha cards keep their key; modulation does not", "[m2]") {
    // The key is how a black-backed card survives being drawn opaquely, which
    // is still what happens for every non-additive mode.
    CHECK(m2BatchWantsColorKey(0, true));
    CHECK(m2BatchWantsColorKey(1, true));
    CHECK(m2BatchWantsColorKey(2, true));
    CHECK_FALSE(m2BatchWantsColorKey(5, true));
    CHECK_FALSE(m2BatchWantsColorKey(6, true));

    // And a texture nothing marked is never keyed, whatever it blends as.
    for (uint8_t mode = 0; mode <= 6; ++mode) {
        INFO("blend mode " << int(mode));
        CHECK_FALSE(m2BatchWantsColorKey(mode, false));
    }
}

TEST_CASE("opaque mount skin does not inherit atlas transparency", "[m2][mount]") {
    using wowee::rendering::characterBatchNeedsAlphaTest;
    CHECK_FALSE(characterBatchNeedsAlphaTest(0, true, false, false));
    CHECK_FALSE(characterBatchNeedsAlphaTest(0, true, false, true));
    CHECK(characterBatchNeedsAlphaTest(1, true, false, false));
    CHECK(characterBatchNeedsAlphaTest(0, true, true, false));
    CHECK_FALSE(characterBatchNeedsAlphaTest(2, true, false, false));
    for (uint16_t mode : {3, 4, 6, 7}) {
        CHECK_FALSE(characterBatchNeedsAlphaTest(mode, false, false, false));
    }
}

using wowee::rendering::M2BatchMaterialInputs;
using wowee::rendering::M2BlendPipeline;
using wowee::rendering::m2BatchDrawState;
using wowee::rendering::m2BlendPipeline;
using wowee::rendering::m2DrawBlendMode;

TEST_CASE("a spell effect's Mod layer darkens rather than glows", "[m2]") {
    // Mod is dst * src and only darkens. Spell effects had it remapped to
    // additive along with their opaque layers, so smoke and void effects lit
    // up instead.
    CHECK(m2BlendPipeline(m2DrawBlendMode(5, true)) == M2BlendPipeline::Modulate);
    CHECK(m2BlendPipeline(m2DrawBlendMode(6, true)) == M2BlendPipeline::Modulate2x);
}

TEST_CASE("a spell effect's opaque and keyed layers are still added", "[m2]") {
    // They are painted on black; drawn opaque the black would show.
    CHECK(m2BlendPipeline(m2DrawBlendMode(0, true)) == M2BlendPipeline::AlphaAdd);
    CHECK(m2BlendPipeline(m2DrawBlendMode(1, true)) == M2BlendPipeline::AlphaAdd);
    // Outside an effect they draw as authored.
    CHECK(m2BlendPipeline(m2DrawBlendMode(0, false)) == M2BlendPipeline::Opaque);
}

TEST_CASE("NoAlphaAdd does not share Add's alpha-scaled pipeline", "[m2]") {
    // Mode 3 adds the whole texel whatever its alpha says; a mode 3 texture
    // with an empty alpha channel vanished on the SRC_ALPHA pipeline.
    CHECK(m2BlendPipeline(3) == M2BlendPipeline::NoAlphaAdd);
    CHECK(m2BlendPipeline(4) == M2BlendPipeline::AlphaAdd);
    // Including inside a spell effect.
    CHECK(m2BlendPipeline(m2DrawBlendMode(3, true)) == M2BlendPipeline::NoAlphaAdd);
}

TEST_CASE("a flame card's black-backed layer is added, not cut out", "[m2]") {
    M2BatchMaterialInputs in;
    in.blendMode = 0;
    in.colorKeyBlack = true;
    in.flameCard = true;
    const auto s = m2BatchDrawState(in);
    CHECK_FALSE(s.forceCutout);
    CHECK(m2BlendPipeline(s.drawBlendMode) == M2BlendPipeline::AlphaAdd);
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

TEST_CASE("ground clutter is cut out and lit", "[m2]") {
    M2BatchMaterialInputs in;
    in.blendMode = 0;
    in.groundDetail = true;
    const auto s = m2BatchDrawState(in);
    CHECK(s.forceCutout);
    CHECK(s.alphaTest == 3);
    CHECK(s.litOverride);
}

TEST_CASE("a keyed Mod card keys hard, the same card in an effect does not", "[m2]") {
    M2BatchMaterialInputs in;
    in.blendMode = 5;
    in.colorKeyBlack = true;
    CHECK(m2BatchDrawState(in).colorKeyThreshold == 0.7f);
    in.spellEffect = true;
    CHECK(m2BatchDrawState(in).colorKeyThreshold == 0.08f);
}

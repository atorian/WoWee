// Which part of an asset path a token check is allowed to read.
//
// Model and texture names arrive as full asset paths. A token matched against
// the whole path is really matching the directory tree the artist filed the
// asset under, which has nothing to do with what the asset is.
//
// This was found once already, for models: every model in a
// PassiveDoodads/Lights directory matched the "light" lantern token and wall
// torches turned into floating glow sprites. The fix went into the model
// classifier and not into the texture colour-key hint beside it, which kept
// reading whole paths.
//
// The oracle is the shipped asset list. Of the 4929 textures whose path
// contains one of the colour-key tokens, 3338 contain it only in a directory
// name - 3174 of those being every character leg and boot component, because
// Item/TextureComponents/LegLowerTexture spells "glow". The black key discards
// every pixel darker than its threshold, so those textures were losing their
// dark areas: holes in dark boots, in Scholomance candelabras, in Hellfire
// Citadel statues, and in the logs of a campfire.
#include <catch_amalgamated.hpp>

#include <string>

#include "pipeline/m2_model_classifier.hpp"

using wowee::pipeline::assetTokenName;

TEST_CASE("the token name is the file, not the path", "[asset-token]") {
    CHECK(assetTokenName("World\\Azeroth\\Elwynn\\Trees\\ElwynnTree01.m2") ==
          "elwynntree01");
    CHECK(assetTokenName("world/azeroth/elwynn/trees/elwynntree01.m2") ==
          "elwynntree01");
    // No directory at all, and no extension.
    CHECK(assetTokenName("ElwynnTree01") == "elwynntree01");
    CHECK(assetTokenName("") == "");
}

TEST_CASE("the extension is not part of the name", "[asset-token]") {
    // Rules that look at how a name ends would otherwise be reading ".m2".
    CHECK(assetTokenName("a\\b\\Tree.m2") == "tree");
    CHECK(assetTokenName("a\\b\\Tree.mdx") == "tree");
    CHECK(assetTokenName("a\\b\\Bark.blp") == "bark");
    // Anything else is left alone rather than guessed at.
    CHECK(assetTokenName("a\\b\\Notes.txt") == "notes.txt");
}

// ---------------------------------------------------------------------------
// The same fault in a model name: "fire" inside "hellfire".
//
// classifyM2Model asked has(name, "fire"), so HellfireSkyBox classified as a
// brazier - and the renderer gives an additive batch of a brazier a lamp
// flicker keyed on the instance position. A sky dome's position is the
// camera's, rewritten every frame, and lampFlicker quantises its seed to a
// one-unit grid: every cell the camera crossed re-rolled the phase. Outland's
// sky strobed whenever the camera moved or turned, on its additive layers
// alone, and held still when the camera did.
//
// The file already had this shape once, for "forge" inside "Ironforge".
TEST_CASE("a sky is not a brazier because its name ends in fire",
          "[asset-token][classifier]") {
    const glm::vec3 lo(-181.0f, -181.0f, -110.0f);
    const glm::vec3 hi(181.0f, 181.0f, 145.0f);
    const auto sky = wowee::pipeline::classifyM2Model(
        "Environment\\Stars\\HellfireSkyBox.mdx", lo, hi, 1761, 0);
    CHECK_FALSE(sky.isBrazierOrFire);

    // Something actually named for a fire still is one.
    const glm::vec3 small(-1.0f, -1.0f, 0.0f);
    const glm::vec3 big(1.0f, 1.0f, 2.0f);
    CHECK(wowee::pipeline::classifyM2Model(
        "World\\Doodads\\Campfire01.m2", small, big, 200, 1).isBrazierOrFire);
    CHECK(wowee::pipeline::classifyM2Model(
        "World\\Doodads\\Fire_Large.m2", small, big, 200, 1).isBrazierOrFire);
}

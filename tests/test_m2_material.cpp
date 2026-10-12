#include <catch_amalgamated.hpp>
#include "test_support.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_material.hpp"
#include "rendering/m2_blend_mode.hpp"

using namespace wowee::rendering;

TEST_CASE("texture coordinate combos preserve environment mapping", "[m2][material]") {
    const std::vector<int16_t> coords{0, -1, 1};
    CHECK(m2TextureCoordSet(coords, 0) == 0);
    CHECK(m2TextureCoordSet(coords, 1) == -1);
    CHECK(m2TextureCoordSet(coords, 2) == 1);
    CHECK(m2TextureCoordSet(coords, UINT16_MAX) == 0);
    CHECK(m2TextureCoordSet({}, 0) == 0);
}

TEST_CASE("modulation uses destination RGB and keeps its alpha", "[m2][material]") {
    for (bool twice : {false, true}) {
        const auto state = m2ModulateBlendState(twice);
        CHECK(state.blendEnable == VK_TRUE);
        CHECK(state.srcColorBlendFactor == VK_BLEND_FACTOR_DST_COLOR);
        CHECK(state.dstColorBlendFactor == (twice ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_ZERO));
        CHECK(state.colorBlendOp == VK_BLEND_OP_ADD);
        CHECK(state.srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO);
        CHECK(state.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(state.alphaBlendOp == VK_BLEND_OP_ADD);
    }
}

TEST_CASE("Dalaran sculpture has four reflection layers using Mod2x", "[m2][material][asset]") {
    const std::string path = "Data/expansions/wotlk/world/expansion02/doodads/dalaran/dalaran_sculpture";
    const auto data = wowee::test::readFile(path + ".m2");
    if (data.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    auto model = wowee::pipeline::M2Loader::load(data);
    const auto skin = wowee::test::readFile(path + "00.skin");
    REQUIRE_FALSE(skin.empty());
    REQUIRE(wowee::pipeline::M2Loader::loadSkin(skin, model));
    REQUIRE(model.batches.size() == 14);
    REQUIRE(model.textureCoordLookup == std::vector<int16_t>{0, -1});
    const auto order = m2MaterialDrawOrder(model.batches);
    REQUIRE(order.size() == model.batches.size());
    CHECK(order.front() == 12); // inner symbol is behind the golden shell
    CHECK(order[1] == 0); // equal priorities retain authored layer order
    CHECK(std::find(order.begin(), order.end(), 12) < std::find(order.begin(), order.end(), 10));
    CHECK(std::find(order.begin(), order.end(), 10) < std::find(order.begin(), order.end(), 11));
    const std::vector<size_t> layers{2, 5, 8, 11};
    for (const auto index : layers) {
        const auto& batch = model.batches[index];
        REQUIRE(batch.materialIndex < model.materials.size());
        CHECK(model.materials[batch.materialIndex].blendMode == M2_BLEND_MODULATE2X);
        CHECK(m2TextureCoordSet(model.textureCoordLookup, batch.textureUnit) == -1);
        CHECK_FALSE(m2BatchNeedsAlphaTest(M2_BLEND_MODULATE2X, false));
        CHECK_FALSE(m2BatchWantsColorKey(M2_BLEND_MODULATE2X, true));
        REQUIRE(batch.textureIndex < model.textureLookup.size());
        const auto texture = model.textureLookup[batch.textureIndex];
        REQUIRE(texture < model.textures.size());
        const auto& name = model.textures[texture].filename;
        CHECK((name.find("ARMORREFLECT4") != std::string::npos ||
               name.find("ORBREFLECTBRIGHT") != std::string::npos));
        // A reflection layer covers the same geometry as its preceding base.
        CHECK(batch.indexStart == model.batches[index - 1].indexStart);
        CHECK(batch.indexCount == model.batches[index - 1].indexCount);
    }
}

TEST_CASE("authored priorities sort stably without reordering the batches", "[m2][material]") {
    struct Batch { int8_t priorityPlane; };
    const std::vector<Batch> batches{{0}, {2}, {-1}, {0}, {2}, {-2}};
    CHECK(m2MaterialDrawOrder(batches) == std::vector<size_t>{5, 2, 0, 3, 1, 4});
    CHECK(batches[1].priorityPlane == 2);
    CHECK(m2MaterialDrawOrder(std::vector<Batch>{}).empty());
}

TEST_CASE("transparency combos resolve reordered and invisible tracks", "[m2][material]") {
    const std::vector<float> weights{0.0f, 0.25f, 0.75f};
    const std::vector<uint16_t> lookup{2, 0, 1, UINT16_MAX};
    CHECK(m2TextureWeightOpacity(weights, lookup, 0) == 0.75f);
    CHECK(m2TextureWeightOpacity(weights, lookup, 1) == 0.0f);
    CHECK(m2TextureWeightOpacity(weights, lookup, 2) == 0.25f);
    CHECK(m2TextureWeightOpacity(weights, lookup, 3) == 1.0f);
    CHECK(m2TextureWeightOpacity(weights, lookup, UINT16_MAX) == 1.0f);
    CHECK(m2TextureWeightOpacity({}, lookup, 0) == 1.0f);
}

TEST_CASE("Dalaran fountains combine scrolling foam with a UV1 border mask", "[m2][material][asset]") {
    for (const auto name : {"dalaran_fountain_01", "dalaran_fountain_02"}) {
        const std::string path = std::string("Data/expansions/wotlk/world/expansion02/doodads/dalaran/") + name;
        const auto data = wowee::test::readFile(path + ".m2");
        if (data.empty()) { SUCCEED("optional WotLK asset is not extracted"); continue; }
        auto model = wowee::pipeline::M2Loader::load(data);
        REQUIRE(wowee::pipeline::M2Loader::loadSkin(wowee::test::readFile(path + "00.skin"), model));
        size_t masks = 0;
        for (const auto& batch : model.batches) {
            REQUIRE(batch.materialIndex < model.materials.size());
            const auto mode = model.materials[batch.materialIndex].blendMode;
            if (!m2UsesSecondTextureMask(batch.shader, batch.textureCount, mode)) continue;
            ++masks;
            REQUIRE(batch.textureIndex + 1u < model.textureLookup.size());
            const auto first = model.textureLookup[batch.textureIndex];
            const auto second = model.textureLookup[batch.textureIndex + 1u];
            REQUIRE(first < model.textures.size());
            REQUIRE(second < model.textures.size());
            CHECK(model.textures[first].filename.find("WATERFOAMLOOP2") != std::string::npos);
            CHECK(model.textures[second].filename.find("T_VFX_BORDER3") != std::string::npos);
            CHECK(m2TextureCoordSet(model.textureCoordLookup, batch.textureUnit) == 0);
            CHECK(m2TextureCoordSet(model.textureCoordLookup, batch.textureUnit + 1u) == 1);
            REQUIRE(batch.textureAnimIndex + 1u < model.textureTransformLookup.size());
            CHECK(model.textureTransformLookup[batch.textureAnimIndex] != UINT16_MAX);
            CHECK(model.textureTransformLookup[batch.textureAnimIndex + 1u] == UINT16_MAX);
        }
        CHECK(masks == 1);
        // Statue reflection stages have a different combiner; multiplying their
        // RGBA as though they were foam masks would damage the opaque bronze.
        for (const auto& batch : model.batches) {
            if (batch.shader == 1)
                CHECK_FALSE(m2UsesSecondTextureMask(batch.shader, batch.textureCount,
                    model.materials[batch.materialIndex].blendMode));
        }
        if (model.batches.size() == 25) {
            const auto& crystal = model.materials[model.batches[8].materialIndex];
            CHECK(crystal.blendMode == M2_BLEND_ADD_ALPHA);
            CHECK((crystal.flags & 0x04) == 0); // only the front contributes light
            CHECK((model.materials[model.batches[4].materialIndex].flags & 0x04) != 0);
            for (const auto pair : {std::pair<size_t, float>{1, 16384.0f / 32767.0f},
                                   {2, 10813.0f / 32767.0f}, {16, 6553.0f / 32767.0f}}) {
                const auto& batch = model.batches[pair.first];
                CHECK(m2TextureWeightOpacity(model.textureWeights, model.textureWeightLookup,
                    batch.transparencyIndex) == Catch::Approx(pair.second));
            }
        }
    }
}

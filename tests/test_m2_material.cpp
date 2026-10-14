#include <catch_amalgamated.hpp>
#include "test_support.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_material.hpp"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_combiner.hpp"

using namespace wowee::rendering;

namespace {
M2BatchShading shadingOf(const wowee::pipeline::M2Model& model, const wowee::pipeline::M2Batch& batch) {
    const auto coordAt = [&](size_t i) -> int16_t {
        return i < model.textureCoordLookup.size() ? model.textureCoordLookup[i] : 0;
    };
    return m2BatchShading(model.globalFlags, model.textureCombinerCombos, batch.shader,
                          batch.textureCount, model.materials[batch.materialIndex].blendMode,
                          coordAt(batch.textureUnit), coordAt(batch.textureUnit + 1u));
}
} // namespace

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

TEST_CASE("NoAlphaAdd adds the whole texel", "[m2][material]") {
    // Blend mode 3: neither factor may be the source alpha.
    const auto state = m2NoAlphaAddBlendState();
    CHECK(state.blendEnable == VK_TRUE);
    CHECK(state.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
    CHECK(state.dstColorBlendFactor == VK_BLEND_FACTOR_ONE);
    CHECK(state.colorBlendOp == VK_BLEND_OP_ADD);
}

TEST_CASE("a batch's sampler wraps as its own texture slot says", "[m2][material]") {
    // Bit 0 tiles U, bit 1 tiles V, a clear bit clamps.
    const auto uOnly = m2TextureSamplerInfo(0x1, 8, 0.0f);
    CHECK(uOnly.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    CHECK(uOnly.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const auto vOnly = m2TextureSamplerInfo(0x2, 8, 0.0f);
    CHECK(vOnly.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CHECK(vOnly.addressModeV == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    const auto clamped = m2TextureSamplerInfo(0x0, 8, 0.0f);
    CHECK(clamped.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CHECK(clamped.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    // Every mip of the image stays reachable.
    CHECK(clamped.maxLod == 7.0f);
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
        CHECK(shadingOf(model, batch).coords0 == M2StageCoords::Environment);
        CHECK_FALSE(m2BatchNeedsAlphaTest(M2_BLEND_MODULATE2X, false));
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
            const auto shading = shadingOf(model, batch);
            // A blended layer whose second stage, on UV1, is combined into it.
            if (mode < 2 || shading.stages != 2 || !m2CombinerUsesSecondStage(shading.combiner) ||
                shading.coords1 != M2StageCoords::Uv1) continue;
            ++masks;
            REQUIRE(batch.textureIndex + 1u < model.textureLookup.size());
            const auto first = model.textureLookup[batch.textureIndex];
            const auto second = model.textureLookup[batch.textureIndex + 1u];
            REQUIRE(first < model.textures.size());
            REQUIRE(second < model.textures.size());
            CHECK(model.textures[first].filename.find("WATERFOAMLOOP2") != std::string::npos);
            CHECK(model.textures[second].filename.find("T_VFX_BORDER3") != std::string::npos);
            CHECK(shading.coords0 == M2StageCoords::Uv0);
            REQUIRE(batch.textureAnimIndex + 1u < model.textureTransformLookup.size());
            CHECK(model.textureTransformLookup[batch.textureAnimIndex] != UINT16_MAX);
            CHECK(model.textureTransformLookup[batch.textureAnimIndex + 1u] == UINT16_MAX);
        }
        CHECK(masks == 1);
        // The opaque bronze takes no alpha from either stage: its first op is
        // Opaque whatever the file's shader id says.
        for (const auto& batch : model.batches) {
            if (model.materials[batch.materialIndex].blendMode != 0) continue;
            const auto c = shadingOf(model, batch).combiner;
            CHECK((c == M2Combiner::Opaque ||
                   (c >= M2Combiner::OpaqueOpaque && c <= M2Combiner::OpaqueAddNA)));
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

TEST_CASE("Dalaran lamp light volumes do not cast opaque shadows", "[m2][material][asset][shadow]") {
    for (const auto name : {"dalaran_lamppost01", "dalaran_lampwall01"}) {
        const std::string path = std::string("Data/expansions/wotlk/world/expansion02/doodads/dalaran/") + name;
        const auto data = wowee::test::readFile(path + ".m2");
        if (data.empty()) { SUCCEED("optional WotLK lamp is not extracted"); continue; }
        auto model = wowee::pipeline::M2Loader::load(data);
        REQUIRE(wowee::pipeline::M2Loader::loadSkin(wowee::test::readFile(path + "00.skin"), model));
        size_t solid = 0, blended = 0;
        for (const auto& batch : model.batches) {
            REQUIRE(batch.materialIndex < model.materials.size());
            const auto blend = model.materials[batch.materialIndex].blendMode;
            const bool casts = m2BatchCastsShadow(blend, 1.0f, false, false);
            if (blend >= 2) { ++blended; CHECK_FALSE(casts); }
            else { ++solid; CHECK(casts); }
        }
        CHECK(solid == (std::string(name) == "dalaran_lamppost01" ? 3 : 2));
        CHECK(blended == (std::string(name) == "dalaran_lamppost01" ? 4 : 2));
    }
    CHECK_FALSE(m2BatchCastsShadow(0, 0.0f, false, false));
    CHECK_FALSE(m2BatchCastsShadow(0, 1.0f, true, false));
    CHECK_FALSE(m2BatchCastsShadow(0, 1.0f, false, true));
}

TEST_CASE("Dalaran's lamp carries a light that comes on with its Opened state",
          "[m2][light][asset]") {
    const auto data = wowee::test::readFile(
        "Data/expansions/wotlk/world/expansion02/doodads/dalaran/dalaran_lamppost01.m2");
    if (data.empty()) { SUCCEED("optional WotLK lamp is not extracted"); return; }
    const auto model = wowee::pipeline::M2Loader::load(data);
    REQUIRE(model.lights.size() == 1);
    const auto& light = model.lights[0];
    CHECK(light.type == 1);
    CHECK(light.bone == 2);
    CHECK(light.position.z == Catch::Approx(8.27f).margin(0.01f));
    // Sequences: 0 Stand, 1 Closed (147), 2 Opening (148), 3 Opened (149).
    REQUIRE(model.sequences.size() == 4);
    REQUIRE(model.sequences[3].id == 149);
    REQUIRE(light.visibility.sequences.size() == 4);
    CHECK(light.visibility.sequences[0].floatValues == std::vector<float>{0.0f});
    CHECK(light.visibility.sequences[1].floatValues == std::vector<float>{0.0f});
    CHECK(light.visibility.sequences[3].floatValues.front() == 1.0f);
    REQUIRE_FALSE(light.attenuationEnd.sequences.empty());
    CHECK(light.attenuationEnd.sequences[0].floatValues.front() == Catch::Approx(8.80f).margin(0.01f));
}

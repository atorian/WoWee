// Texture animation on doodads: how the loader reads it and what the shader is
// handed.
//
// Two things were wrong. The renderer sampled the translation track alone and
// passed it as a UV offset, so a texture animation that rotates or scales
// stood still. And the loader read the rotation keys as the compressed int16
// quaternions bones use, when a texture transform stores full float ones, so
// even a renderer that applied the rotation would have applied noise.
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_uv_transform.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <vector>

using namespace wowee::pipeline;
using namespace wowee::rendering;

namespace {

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
void putF(std::vector<uint8_t>& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }

/// A WotLK (version 264) model whose only content is one texture transform,
/// with a single rotation key and no translation or scale keys.
std::vector<uint8_t> modelWithTextureRotation(float x, float y, float z, float w) {
    constexpr size_t kHeader = 304;
    std::vector<uint8_t> b(kHeader, 0);
    std::memcpy(b.data(), "MD20", 4);
    put32(b, 4, 264);

    const size_t transformAt = b.size();
    b.resize(transformAt + 60, 0);          // translation, rotation, scaling tracks
    put32(b, 96, 1);                        // nUVAnimation
    put32(b, 100, uint32_t(transformAt));   // ofsUVAnimation

    const size_t rotation = transformAt + 20;
    std::memset(b.data() + rotation, 0, 2);   // interpolation: none
    const int16_t noGlobalSequence = -1;
    std::memcpy(b.data() + rotation + 2, &noGlobalSequence, 2);

    const size_t timestampHeader = b.size();
    const size_t keyHeader = timestampHeader + 8;
    const size_t timestamps = keyHeader + 8;
    const size_t keys = timestamps + 4;
    b.resize(keys + 16, 0);
    put32(b, rotation + 4, 1);
    put32(b, rotation + 8, uint32_t(timestampHeader));
    put32(b, rotation + 12, 1);
    put32(b, rotation + 16, uint32_t(keyHeader));
    put32(b, timestampHeader, 1);
    put32(b, timestampHeader + 4, uint32_t(timestamps));
    put32(b, keyHeader, 1);
    put32(b, keyHeader + 4, uint32_t(keys));
    put32(b, timestamps, 0);
    putF(b, keys, x);
    putF(b, keys + 4, y);
    putF(b, keys + 8, z);
    putF(b, keys + 12, w);
    return b;
}

M2AnimationTrack singleQuatKey(const glm::quat& q) {
    M2AnimationTrack track;
    track.sequences.resize(1);
    track.sequences[0].timestamps = {0};
    track.sequences[0].quatValues = {q};
    return track;
}

}  // namespace

TEST_CASE("a texture transform's rotation keys are read as float quaternions", "[m2][texanim]") {
    // A quarter turn about the texture's normal.
    const M2Model model = M2Loader::load(modelWithTextureRotation(0.0f, 0.0f, 0.70710678f, 0.70710678f));
    REQUIRE(model.textureTransforms.size() == 1);
    const auto& sequences = model.textureTransforms[0].rotation.sequences;
    REQUIRE(sequences.size() == 1);
    REQUIRE(sequences[0].quatValues.size() == 1);
    const glm::quat q = sequences[0].quatValues[0];
    CHECK(q.x == Catch::Approx(0.0f).margin(1e-5));
    CHECK(q.y == Catch::Approx(0.0f).margin(1e-5));
    CHECK(q.z == Catch::Approx(0.70710678f).margin(1e-5));
    CHECK(q.w == Catch::Approx(0.70710678f).margin(1e-5));
}

TEST_CASE("a translation slides the texture", "[m2][texanim]") {
    const auto t = m2UVTranslation({0.25f, -0.5f});
    const glm::vec2 uv = m2ApplyUVTransform(t, {0.0f, 0.0f});
    CHECK(uv.x == Catch::Approx(0.25f));
    CHECK(uv.y == Catch::Approx(-0.5f));
}

TEST_CASE("a rotation turns the texture about its centre", "[m2][texanim]") {
    const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    const auto t = m2UVTransform(glm::vec3(0.0f), quarterTurn, glm::vec3(1.0f));
    // The middle of the right-hand edge goes to the middle of the top edge.
    const glm::vec2 edge = m2ApplyUVTransform(t, {1.0f, 0.5f});
    CHECK(edge.x == Catch::Approx(0.5f).margin(1e-5));
    CHECK(edge.y == Catch::Approx(1.0f).margin(1e-5));
    // And the centre stays where it is.
    const glm::vec2 centre = m2ApplyUVTransform(t, {0.5f, 0.5f});
    CHECK(centre.x == Catch::Approx(0.5f).margin(1e-5));
    CHECK(centre.y == Catch::Approx(0.5f).margin(1e-5));
}

TEST_CASE("a scale grows the texture about its centre", "[m2][texanim]") {
    const auto t = m2UVTransform(glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(2.0f, 2.0f, 1.0f));
    const glm::vec2 corner = m2ApplyUVTransform(t, {1.0f, 1.0f});
    CHECK(corner.x == Catch::Approx(1.5f));
    CHECK(corner.y == Catch::Approx(1.5f));
    const glm::vec2 centre = m2ApplyUVTransform(t, {0.5f, 0.5f});
    CHECK(centre.x == Catch::Approx(0.5f));
    CHECK(centre.y == Catch::Approx(0.5f));
}

TEST_CASE("sampling a texture animation applies its rotation track", "[m2][texanim]") {
    M2TextureTransform transform;
    transform.rotation = singleQuatKey(glm::angleAxis(glm::radians(180.0f), glm::vec3(0, 0, 1)));
    const auto t = m2SampleUVTransform(transform, 0, 0.0f, 0.0f, {});
    // A half turn about the centre sends the origin to the far corner.
    const glm::vec2 uv = m2ApplyUVTransform(t, {0.0f, 0.0f});
    CHECK(uv.x == Catch::Approx(1.0f).margin(1e-5));
    CHECK(uv.y == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("a texture animation with no keys leaves the UVs alone", "[m2][texanim]") {
    const auto t = m2SampleUVTransform(M2TextureTransform{}, 0, 0.0f, 0.0f, {});
    CHECK(m2UVTransformIsIdentity(t));
}

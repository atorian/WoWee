// A liquid's animated surface texture, made ready to draw: which frames there
// are, which channel holds the ripple, and the mip chain the water samples.
#include <catch_amalgamated.hpp>

#include "rendering/liquid_ripple_frames.hpp"

#include <map>
#include <string>

using wowee::pipeline::BLPImage;
using wowee::rendering::liquidFramePath;
using wowee::rendering::loadLiquidRippleFrames;

namespace {

/// A frame whose ripple is in the alpha (colour black), as lake_a and
/// ocean_h are, or in the grey (alpha opaque), as fast_a is.
BLPImage frame(int size, bool rippleInAlpha, uint8_t seed) {
    BLPImage img;
    img.width = img.height = size;
    img.data.resize(static_cast<size_t>(size) * size * 4);
    for (int i = 0; i < size * size; ++i) {
        const uint8_t v = static_cast<uint8_t>((i * 37 + seed * 11) & 0xFF);
        uint8_t* p = &img.data[static_cast<size_t>(i) * 4];
        if (rippleInAlpha) {
            p[0] = p[1] = p[2] = 4;
            p[3] = v;
        } else {
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    return img;
}

}  // namespace

TEST_CASE("frame paths count from one", "[liquid][ripple]") {
    CHECK(liquidFramePath("XTextures\\ocean\\ocean_h.%d.blp", 1) == "XTextures\\ocean\\ocean_h.1.blp");
    CHECK(liquidFramePath("XTextures\\ocean\\ocean_h.%d.blp", 30) == "XTextures\\ocean\\ocean_h.30.blp");
    CHECK(liquidFramePath("XTextures\\procWater\\map.blp", 3) == "XTextures\\procWater\\map.blp");
}

TEST_CASE("every frame there is, until one is not", "[liquid][ripple]") {
    std::map<std::string, BLPImage> files;
    for (int f = 1; f <= 5; ++f) files["sea." + std::to_string(f) + ".blp"] = frame(8, true, f);
    std::string why;
    const auto set = loadLiquidRippleFrames("sea.%d.blp",
        [&](const std::string& p) { auto it = files.find(p); return it != files.end() ? it->second : BLPImage{}; },
        why);
    REQUIRE(set);
    CHECK(set->frames == 5);
    CHECK(set->width == 8);
    // 8, 4, 2, 1: a level per halving down to a single texel, every frame in each.
    REQUIRE(set->levels.size() == 4);
    CHECK(set->levels[0].size() == 8u * 8u * 5u);
    CHECK(set->levels[3].size() == 5u);
}

TEST_CASE("the ripple is read from whichever channel carries it", "[liquid][ripple]") {
    std::string why;
    const auto inAlpha = loadLiquidRippleFrames("a.%d",
        [&](const std::string& p) { return p == "a.1" ? frame(4, true, 1) : BLPImage{}; }, why);
    REQUIRE(inAlpha);
    CHECK(inAlpha->fromAlpha);
    // The ripple's values come through: the first texel's alpha.
    CHECK(inAlpha->levels[0][1] == frame(4, true, 1).data[7]);

    const auto inGrey = loadLiquidRippleFrames("g.%d",
        [&](const std::string& p) { return p == "g.1" ? frame(4, false, 1) : BLPImage{}; }, why);
    REQUIRE(inGrey);
    CHECK_FALSE(inGrey->fromAlpha);
}

TEST_CASE("a frame of another size ends the set", "[liquid][ripple]") {
    std::string why;
    const auto set = loadLiquidRippleFrames("m.%d",
        [&](const std::string& p) {
            if (p == "m.1" || p == "m.2") return frame(8, true, 1);
            if (p == "m.3") return frame(16, true, 1);
            return BLPImage{};
        },
        why);
    REQUIRE(set);
    CHECK(set->frames == 2);
}

TEST_CASE("no first frame is no set, and says why", "[liquid][ripple]") {
    std::string why;
    CHECK_FALSE(loadLiquidRippleFrames("none.%d", [](const std::string&) { return BLPImage{}; }, why));
    CHECK(why.find("none.1") != std::string::npos);
    CHECK_FALSE(loadLiquidRippleFrames("", [](const std::string&) { return BLPImage{}; }, why));
}

TEST_CASE("a pattern with no frame number is one frame", "[liquid][ripple]") {
    std::string why;
    const auto set = loadLiquidRippleFrames("still.blp",
        [](const std::string&) { return frame(4, true, 1); }, why);
    REQUIRE(set);
    CHECK(set->frames == 1);
}

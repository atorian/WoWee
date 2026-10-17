// A .dds sidecar beside a .blp, as AssetManager::loadTexture hands it over.
//
// The sidecar is DXT blocks. A caller that asks for blocks (the M2, WMO and
// terrain renderers, which upload them as they are) gets them; every other
// caller reads RGBA8 from `data`, as a .blp gives it. The character renderer
// is one of those, and handed the blocks it uploaded an empty `data`: every
// upscaled glow on a weapon or a creature - Thunderfury's lightning ball
// among them - came out a white card.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <vector>

namespace {

/// A 4x4 DXT5 .dds of one block: every texel the given colour, alpha 255.
std::vector<uint8_t> solidDxt5Dds(uint16_t rgb565) {
    std::vector<uint8_t> out;
    auto u32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    auto fourCC = [&out](const char* cc) { out.insert(out.end(), cc, cc + 4); };
    fourCC("DDS ");
    u32(124);                                   // dwSize
    u32(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);    // caps|height|width|pixelformat|mipcount
    u32(4);                                     // height
    u32(4);                                     // width
    u32(0);                                     // pitch
    u32(0);                                     // depth
    u32(1);                                     // mip count
    for (int i = 0; i < 11; ++i) u32(0);        // reserved
    u32(32);                                    // ddspf.dwSize
    u32(0x4);                                   // DDPF_FOURCC
    fourCC("DXT5");
    for (int i = 0; i < 5; ++i) u32(0);         // bit count + masks
    u32(0x1000);                                // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; ++i) u32(0);         // caps2..4, reserved2

    // Alpha: endpoints 255 and 0, every index 0 -> 255.
    out.push_back(255);
    out.push_back(0);
    out.insert(out.end(), 6, 0);
    // Colour: both endpoints the colour, every index 0.
    for (int i = 0; i < 2; ++i) {
        out.push_back(static_cast<uint8_t>(rgb565 & 0xFF));
        out.push_back(static_cast<uint8_t>(rgb565 >> 8));
    }
    out.insert(out.end(), 4, 0);
    return out;
}

}  // namespace

TEST_CASE("a texture with a .dds sidecar is read as RGBA8 unless the blocks are asked for") {
    // The .blp only has to be there: the sidecar is found beside it.
    const auto root = wowee::test::makeDataRoot("wowee_dds_override", {
        {"item\\objectcomponents\\ammo\\glow.blp", std::vector<uint8_t>(16, 0)},
        {"item\\objectcomponents\\ammo\\glow.dds", solidDxt5Dds(0xF800)},  // pure red
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    const char* path = "Item\\ObjectComponents\\Ammo\\glow.blp";

    SECTION("asked for nothing, it is the decoded pixels") {
        const auto image = assets.loadTexture(path);
        REQUIRE(image.isValid());
        CHECK_FALSE(image.isBlockCompressed());
        CHECK(image.width == 4);
        CHECK(image.height == 4);
        REQUIRE(image.data.size() == 4u * 4u * 4u);
        for (size_t i = 0; i < image.data.size(); i += 4) {
            CHECK(image.data[i + 0] == 255);
            CHECK(image.data[i + 1] == 0);
            CHECK(image.data[i + 2] == 0);
            CHECK(image.data[i + 3] == 255);
        }
    }

    SECTION("asked for the blocks, it is the blocks") {
        wowee::pipeline::setBlockCompressionSupported(true);
        const auto image = assets.loadTexture(path, /*keepCompressed=*/true);
        REQUIRE(image.isValid());
        CHECK(image.isBlockCompressed());
        CHECK(image.compression == wowee::pipeline::BLPCompression::DXT5);
    }
}

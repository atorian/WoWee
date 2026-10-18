// Which light each batch of a WMO group is drawn with, as WoW.exe 12340 picks
// it: by the run of MOBA the batch is in (MOGP's transition, interior and
// exterior counts), its group's flags, its material and the model's path.
//
// Groups are read through the loader, as the renderer reads them, so a loader
// that stops keeping MOGP's counts fails here too.
#include <catch_amalgamated.hpp>

#include "pipeline/wmo_loader.hpp"
#include "rendering/wmo_lighting.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using wowee::pipeline::WMOBatchKind;
using wowee::pipeline::WMOLoader;
using wowee::pipeline::WMOModel;
using wowee::rendering::WmoBatchLighting;
using wowee::rendering::wmoBatchLighting;
using wowee::rendering::WmoLight;
using wowee::rendering::wmoReceivesExteriorShadows;

namespace {

constexpr uint32_t kWindow = 0x20;
constexpr uint32_t kUnlit = 0x1;

void put32(std::vector<uint8_t>& out, uint32_t v) {
    const size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(&out[at], &v, 4);
}

void put16(std::vector<uint8_t>& out, uint16_t v) {
    const size_t at = out.size();
    out.resize(at + 2);
    std::memcpy(&out[at], &v, 2);
}

// A group file: MVER, then a MOGP whose header carries `flags` and the three
// batch counts, holding one MOBA batch per entry of `materials`.
std::vector<uint8_t> groupFile(uint32_t flags, uint16_t trans, uint16_t interior, uint16_t exterior,
                               const std::vector<uint8_t>& materials) {
    std::vector<uint8_t> moba;
    put32(moba, 0x4D4F4241);  // MOBA
    put32(moba, static_cast<uint32_t>(materials.size() * 24));
    for (size_t i = 0; i < materials.size(); ++i) {
        moba.resize(moba.size() + 12, 0);               // bounding box
        put32(moba, static_cast<uint32_t>(i * 3));      // startIndex
        put16(moba, 3);                                 // count
        put16(moba, 0);                                 // minIndex
        put16(moba, 0);                                 // maxIndex
        moba.push_back(0);                              // flags
        moba.push_back(materials[i]);                   // material
    }

    std::vector<uint8_t> file;
    put32(file, 0x4D564552);  // MVER
    put32(file, 4);
    put32(file, 17);
    put32(file, 0x4D4F4750);  // MOGP
    put32(file, static_cast<uint32_t>(68 + moba.size()));
    put32(file, 0);  // group name
    put32(file, 0);  // descriptive name
    put32(file, flags);
    file.resize(file.size() + 24, 0);  // bounding box
    put16(file, 0);  // portalStart
    put16(file, 0);  // portalCount
    put16(file, trans);
    put16(file, interior);
    put16(file, exterior);
    file.resize(12 + 8 + 68, 0);  // padding, fogs, liquid, id, flags2
    file.insert(file.end(), moba.begin(), moba.end());
    return file;
}

// A model of `materialFlags.size()` materials and one group read from `group`.
WMOModel modelWith(uint16_t mohdFlags, const std::vector<uint32_t>& materialFlags,
                   const std::vector<uint8_t>& group) {
    WMOModel model;
    model.flags = mohdFlags;
    for (uint32_t f : materialFlags) {
        wowee::pipeline::WMOMaterial m{};
        m.flags = f;
        model.materials.push_back(m);
    }
    model.groups.resize(1);
    WMOLoader::loadGroup(group, model, 0);
    return model;
}

// The light of every batch of `group`, in MOBA order.
std::vector<WmoBatchLighting> lights(const WMOModel& model, size_t group = 0) {
    const auto& g = model.groups[group];
    std::vector<WmoBatchLighting> out;
    for (size_t i = 0; i < g.batches.size(); ++i) {
        const uint8_t mat = g.batches[i].materialId;
        const uint32_t matFlags = mat < model.materials.size() ? model.materials[mat].flags : 0;
        out.push_back(wmoBatchLighting(model.flags, g.flags, g.batchKind(i), matFlags));
    }
    return out;
}

constexpr WmoBatchLighting kSun{.light = WmoLight::Sun};
constexpr WmoBatchLighting kWindowLight{.light = WmoLight::Window};
constexpr WmoBatchLighting kBaked{.light = WmoLight::VertexColour};
constexpr WmoBatchLighting kBuildingAmbient{.light = WmoLight::BuildingAmbient};

// An interior room with vertex colours: one doorway batch, two walls, and the
// window in the one exterior batch - the shape of Duskwood's inn.
const std::vector<uint8_t> kRoom =
    groupFile(0x2805, 1, 2, 1, {0, 1, 1, 2});
const std::vector<uint32_t> kRoomMaterials = {0, 0, kWindow};

}  // namespace

TEST_CASE("an older building lights a room's batches by the run MOBA puts them in",
          "[wmo-light]") {
    const WMOModel model = modelWith(0, kRoomMaterials, kRoom);
    const auto l = lights(model);
    REQUIRE(l.size() == 4);
    // The doorway: sunlit where its vertex colours' alpha is 1, the room's
    // vertex colours where it is 0.
    CHECK(l[0] == WmoBatchLighting{.light = WmoLight::Sun, .transition = true,
                                   .under = WmoLight::VertexColour});
    // The walls: their vertex colours alone - no sun, no night glow.
    CHECK(l[1] == kBaked);
    CHECK(l[2] == kBaked);
    // The window: the window light, though its group is indoors.
    CHECK(l[3] == kWindowLight);
}

TEST_CASE("no batch of a room takes the sun's shadow, though its window is sunlit",
          "[wmo-light]") {
    // The trees outside cast no shadow into a room: the client binds an
    // interior group only the units' shadow map, on every batch of it.
    const WMOModel model = modelWith(0, kRoomMaterials, kRoom);
    const auto l = lights(model);
    // Its window is sunlit and its doorway half so...
    REQUIRE(l[3] == kWindowLight);
    REQUIRE(l[0].light == WmoLight::Sun);
    // ...and still the sun's map falls on none of the room.
    const uint32_t flags = model.groups[0].flags;
    CHECK_FALSE(wmoReceivesExteriorShadows(flags));
    // Showing the sky outside (MOGP 0x100) does not make it outdoors.
    CHECK_FALSE(wmoReceivesExteriorShadows(flags | 0x100));
}

TEST_CASE("every batch of an outdoor group takes the sun's shadow", "[wmo-light]") {
    for (const uint32_t outdoors : {0x8u, 0x40u}) {
        CHECK(wmoReceivesExteriorShadows((0x2805u & ~0x2000u) | outdoors));
    }
}

TEST_CASE("a unified building lights a room by its group, not by the run", "[wmo-light]") {
    const WMOModel model = modelWith(wowee::pipeline::kWMOModelUnifiedRenderPath,
                                     kRoomMaterials, kRoom);
    const auto l = lights(model);
    REQUIRE(l.size() == 4);
    CHECK(l[0] == WmoBatchLighting{.light = WmoLight::Sun, .transition = true,
                                   .under = WmoLight::BuildingAmbient});
    // The walls take the building's ambient - and so glow after dark if
    // their material is F_SIDN, which vertex colours alone never do.
    CHECK(l[1] == kBuildingAmbient);
    CHECK(l[2] == kBuildingAmbient);
    CHECK(l[3] == kWindowLight);
}

TEST_CASE("a unified building's outdoor group is sunlit unless its material is unlit",
          "[wmo-light]") {
    for (uint32_t outdoor : {0x8u, 0x40u}) {
        const WMOModel model = modelWith(wowee::pipeline::kWMOModelUnifiedRenderPath,
                                         {0, kUnlit, kWindow},
                                         groupFile(0x2004 | outdoor, 0, 3, 0, {0, 1, 2}));
        const auto l = lights(model);
        REQUIRE(l.size() == 3);
        CHECK(l[0] == kSun);
        CHECK(l[1] == kBaked);
        // Outdoors a window is lit by the sun like any wall.
        CHECK(l[2] == kSun);
    }
}

TEST_CASE("an older building's group without vertex colours is sunlit whatever its runs",
          "[wmo-light]") {
    // Orgrimmar's streets: interior groups (0x2841) with no MOCV.
    const WMOModel model = modelWith(0, {0, kUnlit, kWindow},
                                     groupFile(0x2841, 1, 1, 1, {0, 1, 2}));
    const auto l = lights(model);
    REQUIRE(l.size() == 3);
    CHECK(l[0] == kSun);
    CHECK(l[1] == kBaked);
    CHECK(l[2] == kSun);
}

TEST_CASE("an older building's outdoor batches are sunlit even when the material is unlit",
          "[wmo-light]") {
    const WMOModel model = modelWith(0, {kUnlit}, groupFile(0x2805, 0, 0, 1, {0}));
    CHECK(lights(model) == std::vector<WmoBatchLighting>{kSun});
}

TEST_CASE("batches past MOGP's counts are lit as exterior", "[wmo-light]") {
    // A header that lists fewer batches than MOBA holds.
    const WMOModel model = modelWith(0, {0}, groupFile(0x2805, 0, 1, 0, {0, 0}));
    CHECK(lights(model) == std::vector<WmoBatchLighting>{kBaked, kSun});
}

namespace {

// The real model at `root` (without ".wmo") and its groups `groups`, or an
// empty model when the WotLK data is not extracted.
WMOModel realModel(const std::string& root, std::initializer_list<uint32_t> groups) {
    const auto data = wowee::test::readFile(root + ".wmo");
    if (data.empty()) return {};
    WMOModel model = WMOLoader::load(data);
    for (uint32_t gi : groups) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "_%03u.wmo", gi);
        const auto group = wowee::test::readFile(root + suffix);
        REQUIRE_FALSE(group.empty());
        REQUIRE(WMOLoader::loadGroup(group, model, gi));
    }
    return model;
}

}  // namespace

TEST_CASE("Dalaran's stained glass in a room takes the building's light, not the sun",
          "[wmo-light][asset]") {
    const WMOModel model = realModel(
        "Data/expansions/wotlk/world/wmo/northrend/dalaran/nd_dalaran", {29});
    if (model.groups.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    const auto& g = model.groups[29];
    REQUIRE(g.flags == 0x2905);
    size_t glass = 0, doorways = 0;
    const auto l = lights(model, 29);
    for (size_t i = 0; i < g.batches.size(); ++i) {
        if (g.batchKind(i) == WMOBatchKind::Transition) {
            ++doorways;
            CHECK(l[i].transition);
            CHECK(l[i].under == WmoLight::BuildingAmbient);
        }
        // EB_DALARAN_WINDOWS1, F_SIDN: in the room's interior run, so mode 3
        // - no sun, and the night glow its shader adds.
        if ((model.materials[g.batches[i].materialId].flags & 0x10) == 0) continue;
        ++glass;
        CHECK(g.batchKind(i) == WMOBatchKind::Interior);
        CHECK(l[i] == kBuildingAmbient);
    }
    CHECK(doorways == 9);
    CHECK(glass == 1);
}

TEST_CASE("Duskwood inn's windows take the window light inside their rooms",
          "[wmo-light][asset]") {
    const WMOModel model = realModel(
        "Data/expansions/wotlk/world/wmo/azeroth/buildings/duskwood_inn/duskwood_inn", {4});
    if (model.groups.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    const auto& g = model.groups[4];
    REQUIRE(g.batches.size() == 6);
    const auto l = lights(model, 4);
    for (size_t i = 0; i < 5; ++i) CHECK(l[i] == kBaked);
    REQUIRE((model.materials[g.batches[5].materialId].flags & kWindow) != 0);
    CHECK(l[5] == kWindowLight);
}

TEST_CASE("Orgrimmar's streets stay sunlit inside portal groups", "[wmo-light][asset]") {
    const WMOModel model = realModel(
        "Data/expansions/wotlk/world/wmo/kalimdor/ogrimmar/ogrimmar", {1});
    if (model.groups.empty()) { SUCCEED("optional WotLK asset is not extracted"); return; }
    const auto& g = model.groups[1];
    REQUIRE(g.flags == 0x2841);
    for (const auto& light : lights(model, 1)) CHECK(light.light == WmoLight::Sun);
}

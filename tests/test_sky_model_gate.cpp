// When the original client's sky models stand in for its own sky - the star
// dome, sun, moons and clouds - and when they are drawn over it.
//
// WoW.exe 3.3.5a's sky render (0x7f09b0) leaves that group out only under a
// skybox slot that is loaded, above 0.99, and whose LightSkybox row does not
// set flag 2 (read at 0x7f3877). Its slots are kept per LightSkybox row
// (0x7ed713): lights naming the same row add up, two rows are two slots even
// when they name one model. Under anything less the client's sky is drawn and
// the models go over it.
//
// The renderer had its own rule: hide the client's sky once the sky models'
// summed weight reached a half. That took the stars, sun, moons and clouds
// away under the Northrend auroras, which ask for them, and at every border
// where two skies crossfade.
//
// Each case is a place made up here - Light, LightParams and LightSkybox
// files in a data root - that LightingManager reads as it reads the client's,
// and walks into.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/lighting_manager.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using wowee::rendering::LightingManager;

namespace {

constexpr uint32_t OPAQUE_SKY = 1;       // a sky that stands in for the client's
constexpr uint32_t OTHER_OPAQUE_SKY = 2;
constexpr uint32_t AURORA = 26;          // flags 2, as Aurora.mdx's row has
constexpr uint32_t FIRE_SKY = 39;        // DeathKnightFireSkyBox, flags 0
constexpr uint32_t FIRE_SKY_OVER = 40;   // DeathKnightFireSkyBox again, flags 2

/// One light volume: a map, where it is in the file's own units, and the
/// LightParams row it names in clear weather.
struct Volume {
    uint32_t lightId;
    uint32_t mapId;
    float dbcX, dbcY;
    uint32_t lightParamsId;
};

constexpr float INNER_YARDS = 100.0f;
constexpr float OUTER_YARDS = 200.0f;

std::vector<uint8_t> lightDbc(const std::vector<Volume>& volumes) {
    std::vector<std::vector<uint32_t>> rows;
    for (const Volume& v : volumes) {
        std::vector<uint32_t> row(15, 0);
        row[0] = v.lightId;
        row[1] = v.mapId;
        row[2] = std::bit_cast<uint32_t>(v.dbcX);
        row[3] = std::bit_cast<uint32_t>(0.0f);
        row[4] = std::bit_cast<uint32_t>(v.dbcY);
        row[5] = std::bit_cast<uint32_t>(INNER_YARDS * wowee::rendering::LIGHT_COORD_UNITS_PER_YARD);
        row[6] = std::bit_cast<uint32_t>(OUTER_YARDS * wowee::rendering::LIGHT_COORD_UNITS_PER_YARD);
        row[7] = v.lightParamsId;
        rows.push_back(row);
    }
    return wowee::test::wdbcFile(15, rows, std::string(1, '\0'));
}

/// LightParams rows naming a LightSkybox row each: {lightParamsId, skyboxId}.
std::vector<uint8_t> lightParamsDbc(const std::vector<std::pair<uint32_t, uint32_t>>& params) {
    std::vector<std::vector<uint32_t>> rows;
    for (const auto& [id, skybox] : params) rows.push_back({id, 0, skybox, 0, 0, 0, 0, 0, 0});
    return wowee::test::wdbcFile(9, rows, std::string(1, '\0'));
}

struct SkyRow {
    uint32_t id;
    std::string path;
    uint32_t flags;
};

std::vector<uint8_t> lightSkyboxDbc(const std::vector<SkyRow>& skies) {
    std::string strings(1, '\0');
    std::vector<std::vector<uint32_t>> rows;
    for (const SkyRow& s : skies) {
        rows.push_back({s.id, static_cast<uint32_t>(strings.size()), s.flags});
        strings += s.path;
        strings += '\0';
    }
    return wowee::test::wdbcFile(3, rows, strings);
}

/// The client's rows, as far as these cases need them.
const std::vector<SkyRow> kSkies = {
    {OPAQUE_SKY, "Environments\\Stars\\HellfireSkyBox.mdx", 0},
    {OTHER_OPAQUE_SKY, "Environments\\Stars\\NetherstormSkyBox.mdx", 0},
    {AURORA, "ENVIRONMENTS\\Stars\\Aurora.mdx", 2},
    {FIRE_SKY, "ENVIRONMENTS\\Stars\\DeathKnightFireSkyBox.mdx", 0},
    {FIRE_SKY_OVER, "ENVIRONMENTS\\Stars\\DeathKnightFireSkyBox.mdx", 2},
};

/// A LightingManager over a data root holding these volumes, with
/// LightParams row N naming LightSkybox row N.
struct Place {
    wowee::pipeline::AssetManager assets;
    LightingManager lighting;

    Place(const char* name, const std::vector<Volume>& volumes) {
        std::vector<std::pair<uint32_t, uint32_t>> params;
        for (const SkyRow& s : kSkies) params.emplace_back(s.id, s.id);
        const auto root = wowee::test::makeDataRoot(name, {
            {"dbfilesclient\\light.dbc", lightDbc(volumes)},
            {"dbfilesclient\\lightparams.dbc", lightParamsDbc(params)},
            {"dbfilesclient\\lightskybox.dbc", lightSkyboxDbc(kSkies)},
        });
        REQUIRE(assets.initialize(root.string()));
        REQUIRE(lighting.initialize(&assets));
    }

    /// One frame at the centre of the volume at (dbcX, dbcY).
    void step(uint32_t mapId, float dbcX, float dbcY) {
        lighting.update(wowee::rendering::lightPositionToWorld(dbcX, dbcY, 0.0f), mapId, 0,
                        12.0f);
    }

    /// Standing there for a second and a half, long enough for the skies the
    /// lights name to finish fading in. The fade runs on real time.
    void stand(uint32_t mapId, float dbcX, float dbcY) {
        for (int i = 0; i < 6; ++i) {
            step(mapId, dbcX, dbcY);
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
        }
        step(mapId, dbcX, dbcY);
    }

    /// What the renderer asks, with every sky model loaded.
    bool proceduralSkyHidden() const {
        return lighting.skyModelsHideProceduralSky([](uint32_t) { return true; });
    }

    float weightOf(uint32_t skyboxId) const {
        for (const auto& l : lighting.getSkyboxLayers())
            if (l.id == skyboxId) return l.weight;
        return 0.0f;
    }
};

}  // namespace

TEST_CASE("A sky model fully up stands in for the client's own sky", "[sky]") {
    Place place("wowee_sky_gate_opaque", {{1, 0, 1000.0f, 1000.0f, OPAQUE_SKY}});

    // Just arrived: the model is fading in, and the client's sky is drawn
    // under it until it is all the way up.
    place.step(0, 1000.0f, 1000.0f);
    REQUIRE(place.weightOf(OPAQUE_SKY) > 0.0f);
    CHECK_FALSE(place.proceduralSkyHidden());

    place.stand(0, 1000.0f, 1000.0f);
    REQUIRE(place.weightOf(OPAQUE_SKY) > 0.99f);
    CHECK(place.proceduralSkyHidden());

    // Unless its model never loaded: then there is nothing in its place.
    CHECK_FALSE(place.lighting.skyModelsHideProceduralSky([](uint32_t) { return false; }));
}

TEST_CASE("A sky model with LightSkybox flag 2 is drawn over the client's own sky",
          "[sky]") {
    Place place("wowee_sky_gate_aurora", {{1, 0, 1000.0f, 1000.0f, AURORA}});
    place.stand(0, 1000.0f, 1000.0f);

    // The aurora is all the way up, and the stars, sun, moons and clouds
    // stay under it.
    REQUIRE(place.weightOf(AURORA) > 0.99f);
    CHECK_FALSE(place.proceduralSkyHidden());
}

TEST_CASE("Two skies crossfading at a border are both drawn over the client's own sky",
          "[sky]") {
    // Two volumes over one spot, each naming a different opaque sky: each is
    // half of what is overhead, and neither covers the sky alone.
    Place place("wowee_sky_gate_border", {
        {1, 0, 1000.0f, 1000.0f, OPAQUE_SKY},
        {2, 0, 1000.0f, 1000.0f, OTHER_OPAQUE_SKY},
    });
    place.stand(0, 1000.0f, 1000.0f);

    REQUIRE(place.weightOf(OPAQUE_SKY) > 0.4f);
    REQUIRE(place.weightOf(OTHER_OPAQUE_SKY) > 0.4f);
    CHECK_FALSE(place.proceduralSkyHidden());
}

TEST_CASE("Lights naming the same LightSkybox row add up to one sky", "[sky]") {
    // Two volumes, two LightParams rows, one sky: it is the whole of what is
    // overhead, and stands in for the client's.
    Place place("wowee_sky_gate_same_row", {
        {1, 0, 1000.0f, 1000.0f, OPAQUE_SKY},
        {2, 0, 1000.0f, 1000.0f, OPAQUE_SKY},
    });
    place.stand(0, 1000.0f, 1000.0f);

    CHECK(place.lighting.getSkyboxLayers().size() == 1);
    CHECK(place.weightOf(OPAQUE_SKY) > 0.99f);
    CHECK(place.proceduralSkyHidden());
}

TEST_CASE("Two LightSkybox rows naming one model are two skies", "[sky]") {
    // DeathKnightFireSkyBox is row 39 without flag 2 and row 40 with it. Each
    // is half of what is overhead, and neither covers the client's sky alone.
    Place place("wowee_sky_gate_shared_model", {
        {1, 0, 1000.0f, 1000.0f, FIRE_SKY},
        {2, 0, 1000.0f, 1000.0f, FIRE_SKY_OVER},
    });
    place.stand(0, 1000.0f, 1000.0f);

    CHECK(place.lighting.getSkyboxLayers().size() == 2);
    CHECK(place.weightOf(FIRE_SKY) > 0.4f);
    CHECK(place.weightOf(FIRE_SKY_OVER) > 0.4f);
    CHECK_FALSE(place.proceduralSkyHidden());
}

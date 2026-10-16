// The wind: one field over the trees, their shadows, the cloth and the grass,
// drawn from the realm's clock and the zone (WindField, wind_zones.json).
//
// What matters about it is what two players standing side by side see: the
// same wind, from the same clock, whenever each of them logged in - and that
// when it changes it changes smoothly, without a tree jumping.

#include <catch_amalgamated.hpp>

#include "pipeline/wind_zones.hpp"
#include "rendering/wind_field.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>

// The shaders' own wind, run on the CPU: the main pass's path from the
// PerFrame block and the shadow pass's from its push constants.
namespace wind_shader {
using glm::dot;
using glm::length;
using glm::mat3;
using glm::mat4;
using glm::normalize;
using glm::transpose;
using glm::vec2;
using glm::vec3;
using glm::vec4;
using std::cos;
using std::sin;
// What the PerFrame block holds, filled from WindField::gpu() as
// Renderer::updatePerFrameUBO fills it.
vec4 windDirLean, windPhases, windTravel;
#define WIND_PER_FRAME
#include "../assets/shaders/wind.glsli"
#undef WIND_PER_FRAME
}  // namespace wind_shader

using Catch::Approx;
using wowee::pipeline::loadWindZonesFile;
using wowee::pipeline::parseWindZones;
using wowee::pipeline::WindZoneTable;
using wowee::rendering::WindField;
using wowee::rendering::WindInputs;
using wowee::rendering::WindOverride;
using wowee::rendering::WindState;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kFrame = 1.0f / 60.0f;
/// A realm moment: 2025-10-09 08:53:20 UTC.
constexpr double kRealmNow = 1760000000.0;

constexpr const char* kTable = R"({
  // comments are allowed - this is an authored file
  "regimes": {
    "calm":   { "speed": [0.0, 0.5],  "gusts": { "perMinute": 0, "strength": 1.0 } },
    "light":  { "speed": [1.0, 3.0],  "gusts": { "perMinute": 1, "strength": 1.3 } },
    "strong": { "speed": [4.0, 7.0],  "gusts": { "perMinute": 3, "strength": 1.6 } },
    "storm":  { "speed": [8.0, 14.0], "gusts": { "perMinute": 6, "strength": 2.0 } }
  },
  "default": { "direction": 37, "weights": { "light": 1 }, "dwellSeconds": [120, 400] },
  "zones": [
    { "name": "Tanaris", "zones": [440], "direction": 80,
      "weights": { "strong": 1 },
      "gusts": { "perMinuteScale": 1.8, "strengthScale": 1.3 } },
    { "name": "Gadgetzan", "areas": [976], "direction": 200 },
    { "name": "Broken", "zones": [33], "direction": "north" },
    { "name": "Elsewhere", "zones": [33, 1], "direction": 300 }
  ],
  "weather": { "rain": { "favour": "strong" }, "storm": { "force": "storm" } }
})";

WindInputs at(double clock) {
    WindInputs in;
    in.clockSeconds = clock;
    return in;
}

/// Degrees toward which the field blows now.
double headingDegrees(const WindState& s) {
    double deg = std::atan2(s.direction.y, s.direction.x) * 180.0 / kPi;
    return deg < 0.0 ? deg + 360.0 : deg;
}

/// How far a wrapped phase moved forward between two readings.
double advanced(double before, double after, double wrap) {
    return std::fmod(after - before + wrap, wrap);
}

/// Equal as phases: the same point of a cycle, either side of a wrap.
bool samePhase(double a, double b, double wrap, double tolerance) {
    const double d = std::fmod(std::abs(a - b), wrap);
    return std::min(d, wrap - d) <= tolerance;
}

void requireInStep(const WindState& a, const WindState& b, double tolerance) {
    CHECK(a.direction.x == Approx(b.direction.x).margin(1e-6));
    CHECK(a.direction.y == Approx(b.direction.y).margin(1e-6));
    CHECK(a.speed == Approx(b.speed).margin(1e-6));
    CHECK(a.swing == Approx(b.swing).margin(1e-6));
    for (auto [pa, pb] : {std::pair{a.trunkPhase, b.trunkPhase}, {a.branchPhase, b.branchPhase},
                          {a.leafPhase, b.leafPhase}, {a.clothPhase, b.clothPhase},
                          {a.rustlePhase, b.rustlePhase}}) {
        CHECK(samePhase(pa, pb, WindField::kPhaseWrap, tolerance));
    }
    CHECK(samePhase(a.waveTravel, b.waveTravel, WindField::kTravelWrap, tolerance));
    CHECK(samePhase(a.gustTravel, b.gustTravel, WindField::kTravelWrap, tolerance));
}

}  // namespace

TEST_CASE("Two clients reading the same realm clock in the same zone blow the same wind",
          "[wind]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.rowFor(0, 12);  // Elwynn: the default row

    WindField first, second;
    for (int frame = 0; frame < 600; ++frame) {
        const double clock = kRealmNow + frame * static_cast<double>(kFrame);
        first.update(kFrame, at(clock), row, table);
        second.update(kFrame, at(clock), row, table);
    }
    requireInStep(first.state(), second.state(), 1e-12);
    CHECK(headingDegrees(first.state()) == Approx(37.0).margin(1e-4));
}

TEST_CASE("A client that logs in later is in step with one already there", "[wind]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.defaultRow();

    // The first has been in the world for a minute when the second arrives.
    WindField early, late;
    const int minute = 3600;
    for (int frame = 0; frame <= minute; ++frame) {
        early.update(kFrame, at(kRealmNow + frame * static_cast<double>(kFrame)), row, table);
    }
    late.update(kFrame, at(kRealmNow + minute * static_cast<double>(kFrame)), row, table);
    requireInStep(early.state(), late.state(), 1e-4);
}

TEST_CASE("A client that drew the wind on its own clock falls into step when the realm's arrives",
          "[wind]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.defaultRow();

    WindField waited, joined;
    // Five seconds on this machine's clock, an hour away from the realm's.
    for (int frame = 0; frame < 300; ++frame) {
        waited.update(kFrame, at(kRealmNow - 3600.0 + frame * static_cast<double>(kFrame)), row,
                      table);
    }
    waited.update(kFrame, at(kRealmNow), row, table);
    joined.update(kFrame, at(kRealmNow), row, table);
    requireInStep(waited.state(), joined.state(), 1e-6);
}

TEST_CASE("A storm raises the wind smoothly and it arrives in the time promised", "[wind]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.defaultRow();

    WindField field;
    double clock = kRealmNow;
    for (int frame = 0; frame < 60; ++frame, clock += kFrame) field.update(kFrame, at(clock), row, table);
    const float calmSpeed = field.state().speed;
    REQUIRE(calmSpeed == Approx(2.0f));  // light, the middle of 1-3

    WindInputs storm = at(clock);
    storm.weatherType = 3;
    storm.weatherIntensity = 1.0f;
    const float stormSpeed = 11.0f;  // storm, the middle of 8-14

    // No frame moves it by more than the easing allows: the change over the
    // time constant, plus the floor rate.
    const float maxStep = ((stormSpeed - calmSpeed) / WindField::kSpeedTimeConstant +
                           WindField::kSpeedFloorRate) * kFrame + 1e-5f;
    float previous = calmSpeed;
    float afterOneSecond = 0.0f;
    float arrivedAfter = -1.0f;
    for (int frame = 1; frame <= static_cast<int>(WindField::kSpeedSettleSeconds * 60.0f); ++frame) {
        clock += kFrame;
        storm.clockSeconds = clock;
        field.update(kFrame, storm, row, table);
        const float speed = field.state().speed;
        REQUIRE(speed >= previous);
        REQUIRE(speed - previous <= maxStep);
        if (frame == 60) afterOneSecond = speed;
        if (arrivedAfter < 0.0f && speed == Approx(stormSpeed).margin(1e-4)) arrivedAfter = frame * kFrame;
        previous = speed;
    }
    // Not a snap: a second in, it is well short of the storm...
    CHECK(afterOneSecond < stormSpeed - 3.0f);
    // ...and it is there within the settle time.
    CHECK(arrivedAfter > 0.0f);
    CHECK(field.state().speed == Approx(stormSpeed).margin(1e-4));
    // And everything moves harder in it.
    CHECK(field.state().swing > 1.5f);
}

TEST_CASE("A change of speed changes how fast the sway runs, never where it is", "[wind]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.defaultRow();

    WindField field;
    WindInputs in = at(kRealmNow);
    in.fixedWind = WindOverride{37.0f, 2.0f};
    field.update(kFrame, in, row, table);

    auto runSecond = [&](float speed) {
        in.fixedWind->speed = speed;
        double moved = 0.0;
        double biggest = 0.0;
        for (int frame = 0; frame < 60; ++frame) {
            const double before = field.state().leafPhase;
            in.clockSeconds += kFrame;
            field.update(kFrame, in, row, table);
            const double step = advanced(before, field.state().leafPhase, WindField::kPhaseWrap);
            moved += step;
            biggest = std::max(biggest, step);
        }
        return std::pair{moved, biggest};
    };

    const auto [atBreeze, breezeStep] = runSecond(2.0f);
    // Now a gale. If the phase were the clock times a rate, the first frame of
    // it would put the leaves where a gale would have had them all along - a
    // jump of thousands of radians at a realm clock this size. It must be a
    // frame's worth of the rate the wind has reached at most.
    const auto [rampMoved, rampStep] = runSecond(14.0f);
    const auto [atGale, galeStep] = runSecond(14.0f);

    CHECK(rampMoved > atBreeze);
    CHECK(atGale > rampMoved);
    CHECK(atGale > atBreeze * 2.0);
    // No frame of the change moves the leaves further than a frame of the
    // stronger wind that follows it: the change is in the rate, not a jump.
    CHECK(rampStep <= galeStep * 1.001);
    CHECK(breezeStep < rampStep);
}

TEST_CASE("A turning wind turns the short way round", "[wind]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.defaultRow();

    WindField field;
    WindInputs in = at(kRealmNow);
    in.fixedWind = WindOverride{350.0f, 2.0f};
    field.update(kFrame, in, row, table);
    REQUIRE(headingDegrees(field.state()) == Approx(350.0).margin(1e-3));

    // To 10 degrees: twenty degrees through north-of-x, not three hundred and
    // forty back round.
    in.fixedWind->directionDegrees = 10.0f;
    for (int frame = 0; frame < static_cast<int>(WindField::kDirectionSettleSeconds * 60.0f); ++frame) {
        in.clockSeconds += kFrame;
        field.update(kFrame, in, row, table);
        const double heading = headingDegrees(field.state());
        REQUIRE((heading >= 349.99 || heading <= 10.01));
    }
    CHECK(headingDegrees(field.state()) == Approx(10.0).margin(1e-3));
}

TEST_CASE("A zone's row sets its wind and the default blows everywhere else", "[wind][json]") {
    const WindZoneTable table = parseWindZones(kTable, "test");

    // Tanaris names its own direction and blows strong.
    WindField tanaris;
    tanaris.update(kFrame, at(kRealmNow), table.rowFor(0, 440), table);
    CHECK(headingDegrees(tanaris.state()) == Approx(80.0).margin(1e-3));
    CHECK(tanaris.state().speed == Approx(5.5f));

    // A zone the file does not name takes the default.
    WindField elwynn;
    elwynn.update(kFrame, at(kRealmNow), table.rowFor(0, 12), table);
    CHECK(headingDegrees(elwynn.state()) == Approx(37.0).margin(1e-3));
    CHECK(elwynn.state().speed == Approx(2.0f));
}

TEST_CASE("A subzone's row wins over its zone's", "[wind][json]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    // Gadgetzan (976) is in Tanaris (440): its own row, not Tanaris's.
    CHECK(table.rowFor(976, 440).directionDegrees == Approx(200.0f));
    // And it inherits what it does not say from the default, not from Tanaris.
    WindField field;
    field.update(kFrame, at(kRealmNow), table.rowFor(976, 440), table);
    CHECK(field.state().speed == Approx(2.0f));
    // The rest of Tanaris is Tanaris.
    CHECK(table.rowFor(977, 440).directionDegrees == Approx(80.0f));
}

TEST_CASE("A malformed row is left out and the places it names take the next match",
          "[wind][json]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    // "Broken" says its direction is "north", which is not a number of
    // degrees: zone 33 is not its - the next row naming 33 has it.
    CHECK(table.rowFor(0, 33).name == "Elsewhere");
    CHECK(table.rowFor(0, 33).directionDegrees == Approx(300.0f));

    // With nothing after it, a malformed row's zone takes the default.
    const WindZoneTable alone = parseWindZones(R"({
      "regimes": { "light": { "speed": [1.0, 3.0] } },
      "default": { "direction": 37, "weights": { "light": 1 } },
      "zones": [ { "name": "Bad", "zones": [33], "weights": { "hurricane": 1 } } ]
    })", "test");
    CHECK(alone.rowFor(0, 33).name == "default");
    CHECK(alone.rowFor(0, 33).directionDegrees == Approx(37.0f));
}

TEST_CASE("A table that is not there, or not JSON, still blows the default light wind",
          "[wind][json]") {
    for (const WindZoneTable& table :
         {loadWindZonesFile("no/such/wind_zones.json"), parseWindZones("{ not json", "test")}) {
        WindField field;
        field.update(kFrame, at(kRealmNow), table.rowFor(0, 440), table);
        CHECK(headingDegrees(field.state()) == Approx(37.0).margin(1e-3));
        CHECK(field.state().speed == Approx(2.0f));
    }
}

TEST_CASE("Rain makes strong wind as likely as all the rest together", "[wind][json]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const auto& row = table.defaultRow();
    // Over a long run of slots the default row, all "light", blows strong
    // about half the time in rain - and never in dry weather.
    const double slot = 0.5 * (row.dwellMinSeconds + row.dwellMaxSeconds);
    int strongDry = 0, strongWet = 0;
    const int slots = 2000;
    for (int i = 0; i < slots; ++i) {
        const double clock = kRealmNow + (i + 0.5) * slot;
        const auto dry = wowee::rendering::baseWindAt(clock, row, table, 0, 0.0f);
        const auto wet = wowee::rendering::baseWindAt(clock, row, table, 1, 0.8f);
        strongDry += dry.speed > 4.0f ? 1 : 0;
        strongWet += wet.speed > 4.0f ? 1 : 0;
    }
    CHECK(strongDry == 0);
    CHECK(strongWet > slots * 4 / 10);
    CHECK(strongWet < slots * 6 / 10);
}

namespace {

/// What Renderer::updatePerFrameUBO puts in the PerFrame block.
void toPerFrame(const wowee::rendering::WindGpu& gpu) {
    wind_shader::windDirLean = gpu.dirLean;
    wind_shader::windPhases = gpu.phases;
    wind_shader::windTravel = gpu.travel;
}

/// A doodad's model matrix: placed, turned about z, scaled.
glm::mat4 placed(const glm::vec3& at, float yawDegrees, float scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), at);
    m = glm::rotate(m, glm::radians(yawDegrees), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, glm::vec3(scale));
}

}  // namespace

TEST_CASE("A tree and its shadow sway together whichever way it faces", "[wind][shader]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    WindField field;
    WindInputs in = at(kRealmNow);
    in.fixedWind = WindOverride{120.0f, 9.0f};

    const glm::mat4 trees[] = {placed({100.0f, 200.0f, 30.0f}, 0.0f, 1.0f),
                               placed({-4000.0f, 812.0f, 55.0f}, 73.0f, 1.3f),
                               placed({9.0f, -9.0f, 0.0f}, 211.0f, 0.7f)};
    const glm::vec3 vertices[] = {{0.0f, 0.0f, 12.0f}, {2.5f, -1.0f, 18.0f}, {-3.0f, 4.0f, 6.0f}};
    for (int frame = 0; frame < 240; ++frame) {
        field.update(kFrame, in, table.defaultRow(), table);
        in.clockSeconds += kFrame;
        const auto gpu = field.gpu();
        toPerFrame(gpu);
        for (const auto& model : trees) {
            const auto push = wowee::rendering::shadowWindFor(gpu, model);
            const glm::vec2 origin(model[3]);
            for (const auto& v : vertices) {
                const glm::vec2 tree = wind_shader::foliageWindSway(
                    wind_shader::perFrameFoliageWind(model), v, v, origin, 0.8f);
                const glm::vec2 shadow = wind_shader::foliageWindSway(
                    wind_shader::foliageWindFromShadowPush(push.wind, push.phases), v, v, origin, 0.8f);
                REQUIRE(shadow.x == Approx(tree.x).margin(1e-5));
                REQUIRE(shadow.y == Approx(tree.y).margin(1e-5));
            }
        }
    }
}

TEST_CASE("A tree leans downwind whichever way it faces", "[wind][shader]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const float toward = 120.0f;
    const glm::vec2 downwind(std::cos(glm::radians(toward)), std::sin(glm::radians(toward)));

    for (const float yaw : {0.0f, 90.0f, 211.0f}) {
        WindField field;
        WindInputs in = at(kRealmNow);
        in.fixedWind = WindOverride{toward, 9.0f};
        const glm::mat4 model = placed({50.0f, 60.0f, 0.0f}, yaw, 1.0f);
        const glm::vec3 top(0.0f, 0.0f, 20.0f);

        // The swing comes and goes; what is left over a long run is the lean.
        glm::vec2 mean(0.0f);
        const int frames = 60 * 60;
        for (int frame = 0; frame < frames; ++frame) {
            field.update(kFrame, in, table.defaultRow(), table);
            in.clockSeconds += kFrame;
            toPerFrame(field.gpu());
            const glm::vec2 sway = wind_shader::foliageWindSway(
                wind_shader::perFrameFoliageWind(model), top, top, glm::vec2(model[3]), 1.0f);
            mean += glm::vec2(glm::mat3(model) * glm::vec3(sway, 0.0f));
        }
        mean /= static_cast<float>(frames);
        INFO("tree turned " << yaw << " degrees");
        CHECK(glm::length(mean) > 0.1f);
        CHECK(glm::dot(glm::normalize(mean), downwind) > 0.98f);
    }
}

TEST_CASE("Grass bends downwind, and further in a stronger wind", "[wind][shader]") {
    const WindZoneTable table = parseWindZones(kTable, "test");
    const float toward = 300.0f;
    const glm::vec2 downwind(std::cos(glm::radians(toward)), std::sin(glm::radians(toward)));

    auto meanBend = [&](float speed) {
        WindField field;
        WindInputs in = at(kRealmNow);
        in.fixedWind = WindOverride{toward, speed};
        field.update(kFrame, in, table.defaultRow(), table);
        toPerFrame(field.gpu());
        glm::vec2 mean(0.0f);
        int blades = 0;
        for (int x = 0; x < 40; ++x) {
            for (int y = 0; y < 40; ++y) {
                const glm::vec2 root(-9000.0f + x * 1.7f, 300.0f + y * 1.3f);
                const glm::vec3 bend = wind_shader::grassWindBend(
                    wind_shader::perFrameGrassWind(), root, std::fmod(x * 0.37f + y * 0.61f, 1.0f));
                mean += glm::vec2(bend.x, bend.y) * bend.z;
                ++blades;
            }
        }
        return mean / static_cast<float>(blades);
    };

    const glm::vec2 breeze = meanBend(2.0f);
    const glm::vec2 gale = meanBend(12.0f);
    CHECK(glm::dot(glm::normalize(breeze), downwind) > 0.98f);
    CHECK(glm::dot(glm::normalize(gale), downwind) > 0.98f);
    CHECK(glm::length(gale) > glm::length(breeze) * 1.5f);
}

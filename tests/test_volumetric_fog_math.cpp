// The fog volume's layout against the projection the shaders read it with.
//
// The inject pass places each cell at camera + ray(uv) * depth, the ray a
// bilinear mix of four corner rays built on the CPU; every world shader finds
// its cell the other way round, by projecting its position and taking
// clip.xy / clip.w for uv and clip.w for depth. If those two disagree - a sign
// on the flipped Y, the upscaler's jitter left out, a corner in the wrong slot
// - the fog is lit for one place and read at another, which shows as shafts
// that slide against the trees casting them. Nothing else would say so.
#include <catch_amalgamated.hpp>
#include "rendering/volumetric_fog_math.hpp"
#include "rendering/volumetric_medium.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::froxelCornerRays;

namespace medium_math {
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using glm::exp;
using glm::max;
#include "../assets/shaders/volumetric_medium.glsli"
}

TEST_CASE("clear weather supplies thin air without drifting ground fog", "[volumetric][medium]") {
    using namespace wowee::rendering;
    const auto medium = volumetricMediumFor({}, 1, false);
    const auto sample = [&](float height, float banks) {
        return medium_math::volumeMediumAt(height, banks,
            {medium.air, medium.mist, medium.dust}, 1.0f/8, 1.0f/80, kSandAlbedo);
    };
    const auto ground = sample(0, 1);
    CHECK(std::exp(-ground.a * 100) > .95f);
    CHECK(ground.a > 0); // Sun and torch shafts still have scattering air.
    CHECK(sample(0, .2f) == sample(0, 1.8f));
    CHECK(sample(0, 1) == sample(80, 1));
}

TEST_CASE("fog weather adds a neutral low layer and sand adds coloured dust", "[volumetric][medium]") {
    using namespace wowee::rendering;
    const auto sample = [](const WeatherMix& weather, float height) {
        const auto medium = volumetricMediumFor(weather, 1, false);
        return medium_math::volumeMediumAt(height, 1,
            {medium.air, medium.mist, medium.dust}, 1.0f/8, 1.0f/80, kSandAlbedo);
    };
    const auto wet = sample({.mist = 1}, 0);
    CHECK(std::exp(-wet.a * 100) < .5f);
    CHECK(wet.r == Catch::Approx(wet.b));
    CHECK(sample({.mist = 1}, 40).a < wet.a * .1f);
    const auto sand = sample({.sand = 1}, 0);
    CHECK(sand.r > sand.b * 1.5f);
    CHECK(sand.g > sand.b);
    CHECK(sample({.sand = 1}, 40).a > sand.a * .4f);
    CHECK(sample({.rain = 1}, 0).a > sample({}, 0).a);
    CHECK(sample({.rain = 1}, 0).a < wet.a);
}

TEST_CASE("mixed weather conserves scattering energy and density zero clears every layer",
          "[volumetric][medium]") {
    using namespace wowee::rendering;
    const WeatherMix weather{.rain = .5f, .sand = .5f, .mist = .5f};
    for (float density : {0.0f, .65f, 1.0f, 3.0f}) {
        const auto medium = volumetricMediumFor(weather, density, false);
        const auto at = medium_math::volumeMediumAt(8, 1.3f,
            {medium.air, medium.mist, medium.dust}, 1.0f/8, 1.0f/80, kSandAlbedo);
        if (density == 0) CHECK(at == glm::vec4(0));
        else {
            CHECK(at.a > 0);
            CHECK(at.r <= at.a);
            CHECK(at.g <= at.a);
            CHECK(at.b <= at.a);
        }
    }
}

namespace {

glm::vec3 rayAt(const std::array<glm::vec3, 4>& rays, glm::vec2 uv) {
    const glm::vec3 top = glm::mix(rays[0], rays[1], uv.x);
    const glm::vec3 bottom = glm::mix(rays[2], rays[3], uv.x);
    return glm::mix(top, bottom, uv.y);
}

/// Project, then rebuild the point from where the projection put it, as the
/// shaders and the inject pass respectively do.
void checkRoundTrip(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& camPos,
                    const glm::vec3& point) {
    const glm::vec4 clip = proj * view * glm::vec4(point, 1.0f);
    REQUIRE(clip.w > 0.0f);
    const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
    const glm::vec3 rebuilt = camPos + rayAt(froxelCornerRays(view, proj), uv) * clip.w;
    INFO("point (" << point.x << ", " << point.y << ", " << point.z << ") came back as ("
                   << rebuilt.x << ", " << rebuilt.y << ", " << rebuilt.z << ")");
    CHECK(glm::length(rebuilt - point) < 1e-4f * clip.w + 1e-3f);
}

}  // namespace

TEST_CASE("a fog cell is lit where the shaders will read it", "[volumetric]") {
    // The camera the renderer builds: Z up, glm::perspective, Y flipped for Vulkan.
    const glm::vec3 camPos(-8913.0f, 554.0f, 94.0f);
    const glm::mat4 view = glm::lookAt(camPos, camPos + glm::vec3(0.6f, 0.7f, -0.2f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;

    const glm::vec3 points[] = {
        camPos + glm::vec3(30.0f, 40.0f, -8.0f),
        camPos + glm::vec3(300.0f, 120.0f, 25.0f),
        camPos + glm::vec3(2.0f, 3.0f, -1.5f),
        camPos + glm::vec3(-40.0f, 380.0f, -60.0f),
    };
    for (const auto& p : points) checkRoundTrip(view, proj, camPos, p);

    SECTION("with the upscaler's jitter in the projection") {
        glm::mat4 jittered = proj;
        jittered[2][0] += 0.0013f;
        jittered[2][1] -= 0.0007f;
        for (const auto& p : points) checkRoundTrip(view, jittered, camPos, p);
    }
}

TEST_CASE("the corner rays are one yard deep and span the screen the right way up", "[volumetric]") {
    const glm::vec3 camPos(0.0f);
    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(camPos, forward, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.5f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;
    const auto rays = froxelCornerRays(view, proj);

    for (const auto& r : rays) CHECK(glm::dot(r, forward) == Catch::Approx(1.0f));
    // uv (0,0) is the top of the framebuffer once Y is flipped: the upper
    // corners point up, the lower ones down. The volume's rows run the same
    // way as the screen's, which is what the sky's TexCoord lookup assumes.
    CHECK(rays[0].z > 0.0f);
    CHECK(rays[1].z > 0.0f);
    CHECK(rays[2].z < 0.0f);
    CHECK(rays[3].z < 0.0f);
}

namespace atmosphere_math {
using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using glm::exp;
using glm::log;
using glm::max;
#include "../assets/shaders/atmosphere_medium.glsli"
}

TEST_CASE("atmospheric transport is independent of subdivision", "[volumetric][atmosphere]") {
    using namespace atmosphere_math;
    const vec3 light(.3f,.5f,.7f);
    const float extinction = .001f;
    const auto whole = integrateAtmosphereCell(vec4(0,0,0,1),light*extinction,extinction,2000);
    auto split = vec4(0,0,0,1);
    for (int i=0; i<100; ++i)
        split = integrateAtmosphereCell(split,light*extinction,extinction,20);
    CHECK(whole.a == Catch::Approx(std::exp(-2.f)));
    for (int c=0;c<4;++c) CHECK(split[c] == Catch::Approx(whole[c]).margin(1e-5));
    CHECK(whole.r == Catch::Approx(light.r*(1-std::exp(-2.f))));
}

TEST_CASE("vacuum does not manufacture fog over arbitrarily long rays", "[volumetric][atmosphere]") {
    using namespace atmosphere_math;
    const vec4 original(.2f,.4f,.1f,.8f);
    CHECK(integrateAtmosphereCell(original,vec3(0),0,100000) == original);
    const auto thin = integrateAtmosphereCell(vec4(0,0,0,1),vec3(1e-9f),1e-9f,1);
    CHECK(thin.r == Catch::Approx(1e-9f).margin(1e-12f));
}

TEST_CASE("zone midpoint calibrates atmospheric visibility and thickness scales optical depth", "[volumetric][atmosphere]") {
    using namespace atmosphere_math;
    const float beta = atmosphereBeta(200,1800);
    auto density = atmosphereDensity(0,0,1.f/300,beta);
    auto through = integrateAtmosphereCell(vec4(0,0,0,1),vec3(0),density.x+density.y,1000);
    CHECK(through.a == Catch::Approx(.5f));
    density = atmosphereDensity(0,0,1.f/300,atmosphereBeta(100,900));
    through = integrateAtmosphereCell(vec4(0,0,0,1),vec3(0),density.x+density.y,1000);
    CHECK(through.a == Catch::Approx(.25f));
}

TEST_CASE("world height controls the medium independently of the observer", "[volumetric][atmosphere]") {
    using namespace atmosphere_math;
    const auto low = atmosphereDensity(100,0,1.f/300,.001f);
    const auto high = atmosphereDensity(1000,0,1.f/300,.001f);
    CHECK(high.x < low.x*.1f);
    CHECK(high.y > low.y*.8f); // Thin high-altitude air remains above the aerosol layer.
    const auto translated = atmosphereDensity(600,500,1.f/300,.001f);
    CHECK(translated == low);
    const auto disabled = atmosphereDensity(100,0,1.f/300,0);
    CHECK(disabled == vec2(0));
}

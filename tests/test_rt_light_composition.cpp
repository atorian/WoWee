#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

namespace rt_math {
using vec3 = glm::vec3;
using glm::min;
using glm::mix;
using glm::smoothstep;
glm::vec4 rtParams(0.0f);
#include "../assets/shaders/rt_light_composition.glsli"
}

TEST_CASE("Full RT composes occluded ambient and indirect light", "[rt][lighting]") {
    using namespace rt_math;
    const vec3 ambient(.4f, .6f, .8f);
    RtLight r{1.0f, 1.0f, .25f, vec3(.1f, .05f, .02f)};
    rtParams.x = 3;
    const auto full = rtAmbient(r, ambient);
    CHECK(glm::length(full - vec3(.2f, .2f, .22f)) < 1e-6f);
    rtParams.x = 2;
    CHECK(glm::length(rtAmbient(r, ambient) - vec3(.1f, .15f, .2f)) < 1e-6f);
    rtParams.x = 1;
    CHECK(rtAmbient(r, ambient) == ambient);
}

TEST_CASE("Full RT does not occlude traced incoming light a second time", "[rt][lighting]") {
    using namespace rt_math;
    const vec3 ambient(.4f, .6f, .8f), incoming(.1f, .2f, .3f);
    rtParams.x = 3;
    RtLight r{1.0f, 1.0f, .2f, incoming - ambient * .2f};
    CHECK(glm::length(rtAmbient(r, ambient) - incoming) < 1e-6f);
    r.weight = 0;
    CHECK(rtAmbient(r, ambient) == ambient);
}

TEST_CASE("RT shadows retain mapped casters and missing-history fallback", "[rt][shadow]") {
    using namespace rt_math;
    RtLight r{0.0f, 0.0f, 1.0f, vec3(0)};
    CHECK(rtShadow(r, .35f) == .35f);
    r.weight = 1;
    CHECK(rtShadow(r, .35f) == 0);
    r.sun = 1;
    CHECK(rtShadow(r, .35f) == .35f);
    r.weight = .5f;
    r.sun = 0;
    CHECK(rtShadow(r, .8f) == .5f);
}

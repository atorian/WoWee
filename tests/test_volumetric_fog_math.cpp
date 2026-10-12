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

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using wowee::rendering::froxelCornerRays;

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

// The inject shader's own cell geometry, compiled as C++ against stand-ins for
// the uniforms it reads.
namespace fog_cells {
using glm::ivec3;
using glm::ivec4;
using glm::mat4;
using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::exp;
using glm::log;
using glm::max;
using glm::mix;
vec4 volumetricParams;
struct {
    vec4 rayOrigin;
    vec4 rayCorner[4];
    mat4 prevViewProj;
    ivec4 dims;
} vol;
#include "../assets/shaders/volumetric_fog_cells.glsli"
}  // namespace fog_cells

namespace {

/// Fill the stand-in uniforms as VolumetricFog::record() and the per-frame
/// block do, for a medium volume - with last frame's view the same as this one.
void layOutStillVolume(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& camPos) {
    using namespace fog_cells;
    constexpr float kNear = 0.5f, kFar = 400.0f;
    constexpr int kSlices = 64;
    volumetricParams = glm::vec4(1.0f, kNear, 1.0f / std::log(kFar / kNear), kSlices);
    vol.rayOrigin = glm::vec4(camPos, 0.0f);
    const auto rays = froxelCornerRays(view, proj);
    for (int i = 0; i < 4; i++) vol.rayCorner[i] = glm::vec4(rays[i], 0.0f);
    vol.prevViewProj = proj * view;
    vol.dims = glm::ivec4(160, 90, kSlices, 0);
}

/// A cell's value after a run of frames, blended the way the inject pass
/// blends it, sampling a shadow edge across the middle of the cell along
/// `axis` - lit on the near side. Returns the value after every frame.
std::vector<float> settleAcrossEdge(int axis, int frames, bool viewStill) {
    std::vector<float> values;
    float value = 0.0f;
    for (int frame = 0; frame < frames; frame++) {
        const float lit = wowee::rendering::fogCellJitter(frame)[axis] < 0.5f ? 1.0f : 0.0f;
        const float weight =
            frame == 0 ? 0.0f
                       : wowee::rendering::fogHistoryWeight(viewStill ? static_cast<uint32_t>(frame) : 0);
        value = glm::mix(lit, value, weight);
        values.push_back(value);
    }
    return values;
}

float largestStep(const std::vector<float>& values, size_t from) {
    float largest = 0.0f;
    for (size_t i = from + 1; i < values.size(); i++) {
        largest = std::max(largest, std::abs(values[i] - values[i - 1]));
    }
    return largest;
}

}  // namespace

TEST_CASE("a still view reads each cell's history from that cell", "[volumetric]") {
    const glm::vec3 camPos(-8913.0f, 554.0f, 94.0f);
    const glm::mat4 view = glm::lookAt(camPos, camPos + glm::vec3(0.6f, 0.7f, -0.2f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;
    layOutStillVolume(view, proj, camPos);

    const glm::vec3 dims(160.0f, 90.0f, 64.0f);
    for (const glm::ivec3 cell : {glm::ivec3(80, 45, 20), glm::ivec3(0, 0, 32),
                                  glm::ivec3(159, 89, 63), glm::ivec3(17, 60, 40)}) {
        const glm::vec3 coord = fog_cells::volumeCoord(fog_cells::cellHistoryClip(cell));
        // In cells: the texel this cell was written to last frame is at its
        // centre. A twentieth of a cell off is float noise at these
        // coordinates; reading at the jittered sample is up to half a cell.
        const glm::vec3 offCentre = coord * dims - (glm::vec3(cell) + 0.5f);
        INFO("cell (" << cell.x << ", " << cell.y << ", " << cell.z << ") read " << offCentre.x
                      << ", " << offCentre.y << ", " << offCentre.z << " cells off its centre");
        CHECK(std::abs(offCentre.x) < 0.05f);
        CHECK(std::abs(offCentre.y) < 0.05f);
        CHECK(std::abs(offCentre.z) < 0.05f);
    }
}

TEST_CASE("a cell across a shadow edge stops flickering once the view holds still", "[volumetric]") {
    // Two seconds at sixty frames; the last half second is what is on screen.
    constexpr int kFrames = 120;
    constexpr size_t kWatchFrom = 90;
    for (int axis = 0; axis < 3; axis++) {
        INFO("edge across axis " << axis);
        const auto still = settleAcrossEdge(axis, kFrames, true);
        // Two percent of the sunlit air from one frame to the next: toward
        // the sun, where the air takes a dozen times the light, more than that
        // reads as the light itself flickering.
        CHECK(largestStep(still, kWatchFrom) < 0.02f);
    }
}

TEST_CASE("a moving view keeps no more history than a short one", "[volumetric]") {
    // A view that never holds still blends as it always did: more history
    // than this smears the mist behind anything that moves through it.
    CHECK(wowee::rendering::fogHistoryWeight(0) <= 0.9f);
    const auto moving = settleAcrossEdge(2, 120, false);
    const auto still = settleAcrossEdge(2, 120, true);
    CHECK(largestStep(moving, 90) > largestStep(still, 90));
}

TEST_CASE("the view counts as still through camera easing but not a pixel's turn", "[volumetric]") {
    using wowee::rendering::fogViewHeldStill;
    const glm::vec3 camPos(-8913.0f, 554.0f, 94.0f);
    glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;
    const auto raysLooking = [&](const glm::vec3& eye, float yawDegrees) {
        const float yaw = glm::radians(yawDegrees);
        const glm::vec3 forward(std::cos(yaw), std::sin(yaw), -0.2f);
        return froxelCornerRays(glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f)), proj);
    };
    const auto rays = raysLooking(camPos, 40.0f);

    CHECK(fogViewHeldStill(rays, rays, camPos, camPos));
    // A follow camera easing the last millimetre toward where it is going.
    const glm::vec3 eased = camPos + glm::vec3(0.001f, 0.0f, 0.0005f);
    CHECK(fogViewHeldStill(raysLooking(eased, 40.0f), rays, eased, camPos));
    CHECK(fogViewHeldStill(raysLooking(camPos, 40.001f), rays, camPos, camPos));

    // A step, and a turn of about one pixel of a 1080-line screen.
    const glm::vec3 stepped = camPos + glm::vec3(0.05f, 0.0f, 0.0f);
    CHECK_FALSE(fogViewHeldStill(raysLooking(stepped, 40.0f), rays, stepped, camPos));
    CHECK_FALSE(fogViewHeldStill(raysLooking(camPos, 40.065f), rays, camPos, camPos));
}

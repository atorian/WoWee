#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "rendering/local_lights.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>

namespace wowee {
namespace rendering {


// Must match the PerFrame UBO layout in all shaders (std140 alignment)
struct GPUPerFrameData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrix;
    glm::vec4 lightDir;       // xyz = direction, w = how strongly F_SIDN materials glow (wmo_night_glow.hpp)
    glm::vec4 lightColor;     // xyz = color, w = unused
    glm::vec4 ambientColor;   // xyz = color, w = unused
    glm::vec4 viewPos;        // xyz = camera pos, w = unused
    glm::vec4 fogColor;       // xyz = color, w = unused
    glm::vec4 fogParams;      // x = fogStart, y = fogEnd, z = time, w = unused
    glm::vec4 shadowParams;   // x = enabled(0/1), y = unused, z = one shadow-map texel, w = one texel in yards
    // The player, for effects that react to where they are standing: water
    // ripples and the foliage the player brushes past. playerWake trails the
    // player by a fixed time constant, so clutter the player has already walked
    // through springs back over that interval instead of snapping upright.
    glm::vec4 playerPos;      // xyz = player world position, w = horizontal speed (yd/s)
    glm::vec4 playerWake;     // xyz = trailing player position, w = unused
    glm::ivec4 localLightMeta;  // x = how many point lights the buffer at binding 9 holds
    // The fog volume at set 0 binding 2, and how to find a depth in it:
    // x = on (0/1), y = near edge of the first slice in yards,
    // z = 1 / ln(far / near), w = slice count. Off in the reflection pass and
    // the character preview, which bind a neutral volume there.
    glm::vec4 volumetricParams;
    // Last frame's ray traced lighting at set 0 bindings 3 and 4, and how to
    // find a surface in it (rt_lighting.glsli): the view-projection and camera
    // position it was traced with, and params.x = mode, 0 when there is no
    // result to read. Zero in the reflection pass and the character preview.
    glm::mat4 rtViewProj;
    glm::vec4 rtCameraPos;
    glm::vec4 rtParams;
    // Sky colours for water reflections, followed by view-distance metadata.
    // Surface distance fog keeps the zone's start/end and shares the sky's
    // aerial colour correction when aerialParams.w is enabled.
    glm::vec4 fogSkyTop{0.0f}; // rgb = zenith, w = reserved
    glm::vec4 fogSkyMiddle{0.0f}; // w = actual sky capture valid
    glm::vec4 fogSkyBand1{0.0f}; // w = common horizontal geometry limit
    glm::vec4 fogSkyBand2{0.0f};
    // The units' shadow map around the player at set 0 binding 8
    // (contact_shadows.glsli), as the client's 40-yard map.
    glm::mat4 contactSpaceMatrix{0.0f};
    glm::vec4 contactParams{0.0f}; // enabled, texel UV size, depth span in yards, unused
    // The wind, as wind.glsli reads it (WindField::gpu): one field over the
    // trees, the cloth and the grass. The reflection pass copies the whole
    // block, so reflected foliage moves with the real.
    glm::vec4 windDirLean{1.0f, 0.0f, 0.0f, 0.0f}; // xy toward (world, unit), z lean, w swing
    glm::vec4 windPhases{0.0f};  // x trunk, y branch, z leaf phase, w branch/leaf scale
    glm::vec4 windTravel{0.0f};  // x cloth phase, y grass fronts (yd), z grass swells (yd), w rustle phase
    glm::vec4 skySmogColor{0.0f}; // appended: horizon band for water sky reflections
    glm::vec4 aerialSun{0.0f};   // toward visible sun; w daylight weight
    glm::vec4 aerialLight{0.0f}; // neutral illuminated aerosol; w reserved
    glm::vec4 terrainLodParams{0.0f}; // WDL overlap start/end, ready, reserved
    glm::vec4 aerialParams{0.0f}; // xyz reserved, w aerial colour correction enabled
};

// Push constants for the model matrix (most common case)
struct GPUPushConstants {
    glm::mat4 model;
    /// The point lights that reach the chunk being drawn (local_lights.glsli).
    LocalLightSet localLights = kNoLocalLights;
};

/// The WMO's own, which carries where a batch of cloth hangs beside the model
/// matrix: (top z, drop, centre x, centre y) in the model's local space.
struct WMOPushConstants {
    glm::mat4 model;
    glm::vec4 cloth{0.0f};
    /// The point lights that reach the group being drawn (local_lights.glsli).
    LocalLightSet localLights = kNoLocalLights;
};

// Push constants for the terrain's, the buildings' and the characters'
// shadow passes: the light-space and model matrices multiplied on the CPU,
// since nothing in their shadow shaders wants them apart.
//
// The doodads' pass has its own block (M2ShadowPush), drawn instanced with
// its casters' matrices and wind in a buffer; it keeps `flags` at the same
// offset, since shadow.frag reads them for every caster.
struct ShadowPush {
    glm::mat4 lightSpaceModel;
    /// x: sample the texture. y: alpha-test it. z: bend in the wind (the
    /// doodads' pass only). w: where the alpha test cuts, in 255ths; 0 is a
    /// half. A WMO's alpha key cuts at 224, as the original client's does.
    ///
    /// These were in the uniform buffer, and the M2 pass writes that buffer
    /// twice a frame - once for its solid casters, once for its foliage. A
    /// uniform buffer is read when the draw executes rather than when it is
    /// recorded, so the second write decided what the first pass's draws saw,
    /// and one persistently-mapped copy shared between frames in flight let a
    /// CPU write land in the middle of the previous frame's reads. Losing the
    /// alpha test that way turns a leaf cutout into the solid outline of the
    /// canopy. Push constants are recorded into the command buffer with the
    /// draw, so there is nothing left to race.
    glm::ivec4 flags{0};
};
static_assert(offsetof(ShadowPush, flags) == 64, "shadow.frag reads the flags at offset 64");

// Uniform buffer for shadow rendering parameters (matches shader std140 layout)
struct ShadowParamsUBO {
    int32_t useBones;
    int32_t useTexture;
    int32_t alphaTest;
    int32_t foliageSway;
    float windTime;
    float foliageMotionDamp;
};

// Timer utility for performance profiling queries.
// Uses atomics because floor-height queries are dispatched on async threads
// from CameraController while the main thread may read the counters.
struct QueryTimer {
    std::atomic<double>* totalMs = nullptr;
    std::atomic<uint32_t>* callCount = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    QueryTimer(std::atomic<double>* total, std::atomic<uint32_t>* calls)
        : totalMs(total), callCount(calls) {}
    ~QueryTimer() {
        if (callCount) {
            callCount->fetch_add(1, std::memory_order_relaxed);
        }
        if (totalMs) {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            // Relaxed is fine for diagnostics - exact ordering doesn't matter.
            double old = totalMs->load(std::memory_order_relaxed);
            while (!totalMs->compare_exchange_weak(old, old + ms, std::memory_order_relaxed)) {}
        }
    }
};

} // namespace rendering
} // namespace wowee

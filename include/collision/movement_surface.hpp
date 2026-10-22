#pragma once

#include "collision/movement_contact.hpp"
#include <optional>

namespace wowee::collision {

// Caller-owned kinematics for build 12340 surface traversal. Speeds and flags
// come from movement state; dimensions come from the mover, not the camera.
struct SurfaceMovementState {
    glm::vec3 position{0};
    glm::vec3 groundNormal{0};
    glm::vec3 anchorPosition{0};
    glm::vec3 direction{0};
    glm::vec2 direction2D{0};
    uint32_t flags = 0;
    uint32_t flags2 = 0;
    uint32_t anchorElapsedMs = 0;
    uint32_t fallTimeMs = 0;
    float facing = 0;
    float pitch = 0;
    float anchorFacing = 0;
    float anchorPitch = 0;
    float pitchCos = 1;
    float pitchSin = 0;
    float currentSpeed = 0;
    float fallStartZ = 0;
    // Positive downward, negative at the beginning of a jump.
    float fallVelocity = 0;
    float stepStartZ = 0;
    float walkSpeed = 0;
    float runSpeed = 0;
    float runBackSpeed = 0;
    float swimSpeed = 0;
    float swimBackSpeed = 0;
    float flightSpeed = 0;
    float flightBackSpeed = 0;
};

struct SurfaceMovementShape {
    float radius;
    float height;
    float stepHeight;
    bool clientControlled = true;
};
struct SurfaceMovementResult {
    uint32_t elapsedMs;
    // SetTransport request made by TraceSurface, including zero for detach.
    // No value means the routine made no transport request on this path.
    std::optional<uint64_t> transport;
    std::optional<uint32_t> fallResetTime;
};

// Runs one original TraceSurface request on fully supplied collision facets.
// This is a phase, not a frame dispatcher: the caller must handle remaining
// time, falling/swimming, world queries and movement notifications separately.
[[nodiscard]] SurfaceMovementResult traceMovementSurface(
    std::span<const MovementFacet> facets, SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec2& direction,
    float distance, uint32_t elapsedMs);

// Original Fall phase (0x7618b0), including FallDown notifications. The
// caller owns transport frames and dispatches the returned fall-reset event.
[[nodiscard]] SurfaceMovementResult traceMovementFall(
    std::span<const MovementFacet> facets, SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec2& direction,
    float distance, uint32_t timeMs, uint32_t elapsedMs);

// Original Swim phase is also used for flight. Liquid planes are supplied
// facing into the water, as GetMoveFacets prepares them. No transport is attached.
[[nodiscard]] SurfaceMovementResult traceMovementSwim(
    std::span<const MovementFacet> solids, std::span<const MovementFacet> liquids,
    SurfaceMovementState& state, const SurfaceMovementShape& shape,
    const glm::vec3& direction, float distance, uint32_t elapsedMs);

} // namespace wowee::collision

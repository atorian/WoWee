#pragma once

// How far behind its target the third-person camera sits, as the 3.3.5a
// client works it out (CGCamera, WoW.exe 12340: update 0x606f90, collision
// 0x605d60; render spec section 6c).
//
// One boom from the look-at point straight back to the camera. Whatever it
// meets pulls the camera in at once, a ninth of a yard short of it; once
// clear, the camera goes back out to the distance the player chose over two
// seconds on a cosine ease, started again for as long as something is in the
// way. Nothing is moved sideways, and the player's chosen distance is never
// changed by a collision.

#include <algorithm>
#include <cmath>

namespace wowee::rendering {

/// What the camera keeps short of whatever it meets (1/9 yard, 4 inches).
inline constexpr float kCameraBoomPad = 1.0f / 9.0f;
/// How long the camera takes to go back out once clear.
inline constexpr float kCameraBoomEaseSeconds = 2.0f;
/// How fast it moves to a distance the mouse wheel asks for (cameraDistanceMoveSpeed).
inline constexpr float kCameraBoomWheelSpeed = 8.33f;

/// The way back out after a collision.
struct CameraBoomEase {
    bool active = false;
    float from = 0.0f;
    float elapsed = 0.0f;
};

/// Moves `current` - where the camera would be with nothing in the way - one
/// frame toward the distance the player chose: along the ease after a
/// collision, else at the mouse wheel's speed.
inline void cameraBoomApproach(float& current, CameraBoomEase& ease, float wanted, float dt) {
    if (ease.active) {
        ease.elapsed += dt;
        const float t = std::min(ease.elapsed / kCameraBoomEaseSeconds, 1.0f);
        current = ease.from + (wanted - ease.from) * (1.0f - std::cos(3.14159265f * t)) * 0.5f;
        if (t >= 1.0f) ease.active = false;
        return;
    }
    const float step = kCameraBoomWheelSpeed * dt;
    current = current < wanted ? std::min(current + step, wanted) : std::max(current - step, wanted);
}

/// Closer than this from its near plane, the camera fades the player out.
inline constexpr float kCameraCloseFadeDistance = 1.8315f;

/// How opaque the player is drawn with the camera `distance` from the
/// look-at point (render spec 6c, medium): fully closer than 1.83 yards from
/// the near plane, gone at it, on a cosine ease - the camera is never seen
/// inside the body it is pressed against.
inline float cameraCloseFade(float distance, float nearPlane) {
    constexpr float kStart = 1.0f / 360.0f;
    const float t = (distance - nearPlane - kStart) / (kCameraCloseFadeDistance - kStart);
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    return (1.0f - std::cos(3.14159265f * t)) * 0.5f;
}

/// How far to cast the boom: as far as the camera is, or wants to be.
inline float cameraBoomReach(float current, float wanted) {
    return std::max(current, wanted);
}

/// The camera was brought in to `placed`: `current` follows it to just
/// outside, and the ease back out starts again.
inline void cameraBoomPulledIn(float& current, CameraBoomEase& ease, float placed) {
    if (current - placed > kCameraBoomPad) {
        current = placed + kCameraBoomPad;
        ease = {.active = true, .from = current, .elapsed = 0.0f};
    }
}

/// Where the camera goes, given `hit`, how far along the boom the first
/// obstruction is (`reach` or more when there is none).
inline float cameraBoomPlace(float& current, CameraBoomEase& ease, float hit, float reach) {
    float distance = reach;
    if (hit < reach) distance = std::max(hit - kCameraBoomPad, 0.0f);
    const float placed = std::min(distance, current);
    cameraBoomPulledIn(current, ease, placed);
    return placed;
}

} // namespace wowee::rendering

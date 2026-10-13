#pragma once

#include <cstdint>

namespace wowee {
namespace game {

/**
 * Movement flags for player movement
 */
enum class MovementFlags : uint32_t {
    NONE                = 0x00000000,
    FORWARD             = 0x00000001,
    BACKWARD            = 0x00000002,
    STRAFE_LEFT         = 0x00000004,
    STRAFE_RIGHT        = 0x00000008,
    TURN_LEFT           = 0x00000010,
    TURN_RIGHT          = 0x00000020,
    PITCH_UP            = 0x00000040,
    PITCH_DOWN          = 0x00000080,
    WALKING             = 0x00000100,
    ONTRANSPORT         = 0x00000200,
    LEVITATING          = 0x00000400,
    ROOT                = 0x00000800,
    FALLING             = 0x00001000,
    FALLINGFAR          = 0x00002000,
    // 0x00004000 and 0x00008000 are PENDING_STOP and PENDING_STRAFE_STOP,
    // which the server sets during ordinary movement. Water walking and slow
    // fall sat on those two, so a client under either told the server in every
    // packet that it was in a pending stop, and never saw the state it had
    // actually been put into.
    SWIMMING            = 0x00200000,
    ASCENDING           = 0x00400000,
    DESCENDING          = 0x00800000,
    CAN_FLY             = 0x01000000,
    FLYING              = 0x02000000,
    WATER_WALK          = 0x10000000,  // Walk on the water surface
    FEATHER_FALL        = 0x20000000,  // Slow fall, the rogue's safe fall
    HOVER               = 0x40000000,
};

/// The flags that mean the player is under way, as opposed to standing in a
/// state.
///
/// Two sites decide whether the player is moving - the heartbeat throttle in
/// movement_handler and the send cadence in game_handler - and each wrote out
/// its own list with a comment saying the two had to match. They did not:
/// ASCENDING was in both and DESCENDING in only one, so swimming or flying
/// downwards counted as motion for one and as standing still for the other,
/// and the heartbeats a descending player owed were throttled away.
///
/// Vertical motion is motion in both directions or in neither, which is the
/// property that broke here. Nothing that describes a state the player is in
/// rather than a way they are moving belongs in this - WALKING says how they
/// move, not that they do, and ONTRANSPORT moves the floor rather than them.
inline constexpr uint32_t kLocomotionFlags =
    static_cast<uint32_t>(MovementFlags::FORWARD) |
    static_cast<uint32_t>(MovementFlags::BACKWARD) |
    static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
    static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
    static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
    static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
    static_cast<uint32_t>(MovementFlags::ASCENDING) |
    static_cast<uint32_t>(MovementFlags::DESCENDING) |
    static_cast<uint32_t>(MovementFlags::SWIMMING) |
    static_cast<uint32_t>(MovementFlags::FALLING) |
    static_cast<uint32_t>(MovementFlags::FALLINGFAR);

} // namespace game
} // namespace wowee

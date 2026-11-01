#pragma once

#include "game/opcodes.hpp"

#include <cstdint>
#include <optional>

namespace wowee::game {

/// What the server forced on the player's movement: a speed or rate, a mode
/// (root, hover, feather fall, water walk, flight, gravity), a collision
/// height or a knockback - named by the acknowledgement it asks for.
struct ForcedMovement {
    Opcode acknowledgement = Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK;
    uint32_t counter = 0;
    /// The speed, the rate or the height, as the server sent it.
    float value = 0.0f;
    /// For a mode: on or off.
    bool enabled = false;
    /// For a knockback, in the server's axes: direction, horizontal speed and
    /// the vertical speed as the packet carries it.
    struct Launch {
        float directionX = 0.0f;
        float directionY = 0.0f;
        float speed = 0.0f;
        float verticalSpeed = 0.0f;
    };
    std::optional<Launch> launch;
};

/// The client's own mover of the player, as the game asks it.
///
/// When none is given, or it does not move the player, MovementHandler does
/// what it always has: applies and acknowledges forced movement at once and
/// sends a heartbeat on its own timer. While it does, the original client's
/// rules hold: forced movement is queued with the player's input and
/// acknowledged when it is applied, and the mover sends every movement message
/// itself, heartbeats included.
class ClientMover {
public:
    virtual ~ClientMover() = default;
    /// Whether it moves the player and tells the server so.
    [[nodiscard]] virtual bool reportsMovement() const = 0;
    /// True: taken - it will be applied and acknowledged by the mover, and
    /// MovementHandler only keeps what the game itself shows of it.
    [[nodiscard]] virtual bool takeForcedMovement(const ForcedMovement& command) = 0;
};

}  // namespace wowee::game

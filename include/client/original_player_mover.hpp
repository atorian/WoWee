#pragma once

#include "game/client_mover.hpp"
#include "rendering/character_mover.hpp"

#include <memory>

namespace wowee {
namespace collision { class MovementWorld; }
namespace game { class GameHandler; }
namespace pipeline { class GameTables; }
namespace core {

class CVarStore;

/// The player moved by the original 3.3.5a client's movement: build 12340's
/// event queue, ExecuteMovement and its collision against the map the original
/// collects (MovementWorld), and its own movement messages - instead of the
/// camera's physics.
///
/// Off unless the "originalMovement" setting is 1. Then it moves the player
/// wherever it can, and hands the player back to the camera - saying so, once
/// per change - where it cannot: an expansion other than WotLK, a transport,
/// a taxi, an area whose collision failed. While it moves the player it sends
/// every movement message, heartbeats included, and takes the forced
/// movement the server sends (speeds, roots, modes, collision height,
/// knockback) into its queue, acknowledging each when it applies it.
///
/// Not equivalence with the original client. The solver's phases are
/// compared with the original executable offline; what drives them here -
/// the mover's dimensions, mouse steering, walking, the map's link order - is
/// not. See tasks/collision-original-parity.md.
class OriginalPlayerMover final : public rendering::CharacterMover, public game::ClientMover {
public:
    /// The setting: 1 moves the player by the original's rules, 0 does not.
    static constexpr const char* kSetting = "originalMovement";

    explicit OriginalPlayerMover(const CVarStore& settings);
    ~OriginalPlayerMover() override;
    OriginalPlayerMover(const OriginalPlayerMover&) = delete;
    OriginalPlayerMover& operator=(const OriginalPlayerMover&) = delete;

    /// What it moves the player through. Until then it moves nothing.
    void connect(collision::MovementWorld& world, game::GameHandler& game,
                 const pipeline::GameTables& tables);

    [[nodiscard]] bool movesCharacter(const glm::vec3& position, float facingDeg) override;
    [[nodiscard]] rendering::CharacterMotion move(const rendering::CharacterIntent& intent) override;

    [[nodiscard]] bool reportsMovement() const override;
    [[nodiscard]] bool takeForcedMovement(const game::ForcedMovement& command) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace core
}  // namespace wowee

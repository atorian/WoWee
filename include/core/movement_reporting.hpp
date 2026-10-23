#pragma once
#include "game/opcodes.hpp"
#include <cstdint>
#include <optional>

namespace wowee {
namespace collision { struct SurfaceMovementState; struct MovementClock; }
namespace network { class Packet; }
namespace core {
struct MovementReportingState {
    uint32_t unitFlags=0;
    float facing=0,pitch=0;
    std::optional<game::Opcode> pendingTurn,pendingPitch;
    uint32_t turnTimeMs=0,pitchTimeMs=0;
};
class MovementReportConnection {
public:
    virtual ~MovementReportConnection()=default;
    virtual void sendMovement(const network::Packet& packet)=0;
};
class MovementLocalEffects {
public:
    virtual ~MovementLocalEffects()=default;
    virtual void refreshMovementEffects()=0;
    virtual void standUp()=0;
    virtual void updateInput(uint32_t timeMs)=0;
    virtual void movementBecameIdle()=0;
    virtual void notifyMovement(game::Opcode opcode)=0;
};
// Offline build-12340 SendMovementUpdate, for ordinary movement and forced ACKs
// without attached transport or active spline. Teleport, transport-change and
// worldport have separate layouts and are not supported by this adapter.
// True means a message was submitted to the connection, not delivered remotely.
[[nodiscard]] bool reportUnattachedMovement12340(
    const collision::SurfaceMovementState& movement,MovementReportingState& reporting,
    collision::MovementClock& clock,MovementReportConnection& connection,
    uint64_t moverGuid,bool activeMover,uint32_t timeMs,game::Opcode opcode,
    uint32_t ackCounter=0,float value=0);
// Unit callbacks execute synchronously, in original order. Their implementations
// (especially input updates that may enqueue commands) still need integration.
[[nodiscard]] bool processLocalMovementEvent12340(
    const collision::SurfaceMovementState& movement,MovementReportingState& reporting,
    collision::MovementClock& clock,MovementReportConnection& connection,MovementLocalEffects& effects,
    uint64_t moverGuid,bool activeMover,bool player,uint32_t standState,uint32_t timeMs,
    game::Opcode opcode,bool needAck,uint32_t ackCounter=0,float value=0);
[[nodiscard]] bool flushTimedMovementReports12340(
    const collision::SurfaceMovementState& movement,MovementReportingState& reporting,
    collision::MovementClock& clock,MovementReportConnection& connection,
    uint64_t moverGuid,bool activeMover,uint32_t timeMs);
}
}

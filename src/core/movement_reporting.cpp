#include "core/movement_reporting.hpp"
#include "core/movement_status.hpp"
#include "collision/movement_execution.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include <cmath>

namespace wowee::core {
namespace {
bool hasAcknowledgementIndex(game::Opcode opcode) {
    using game::Opcode;
    switch(opcode) {
    case Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:
    case Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK:
    case Opcode::CMSG_FORCE_PITCH_RATE_CHANGE_ACK:
    case Opcode::CMSG_MOVE_HOVER_ACK:
    case Opcode::CMSG_MOVE_FEATHER_FALL_ACK:
    case Opcode::CMSG_MOVE_WATER_WALK_ACK:
    case Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK:
    case Opcode::CMSG_MOVE_SET_CAN_FLY_ACK:
    case Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK:
    case Opcode::CMSG_FORCE_MOVE_ROOT_ACK:
    case Opcode::CMSG_FORCE_MOVE_UNROOT_ACK:
    case Opcode::CMSG_MOVE_KNOCK_BACK_ACK:
    case Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK:
    case Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK:
        return true;
    default:return false;
    }
}
}
bool reportUnattachedMovement12340(
    const collision::SurfaceMovementState& movement,MovementReportingState& reporting,
    collision::MovementClock& clock,MovementReportConnection& connection,
    uint64_t moverGuid,bool activeMover,uint32_t time,game::Opcode opcode,
    uint32_t counter,float value) {
    // The original samples these properties before checking send admission.
    reporting.facing=movement.facing;
    if((movement.flags&0x2200000u)||(movement.flags2&0x20u))reporting.pitch=movement.pitch;
    const bool acknowledgement=hasAcknowledgementIndex(opcode);
    if(!acknowledgement&&opcode!=game::Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER&&!activeMover)return false;
    const auto status=makeUnattachedMovementStatus12340(movement,time);
    auto packet=acknowledgement?game::MovementPacket::buildForceAck(opcode,status,moverGuid,counter,value):
        game::MovementPacket::build(opcode,status,moverGuid);
    // PrepareMovementPacket updates falling-report state before submission.
    if(!(movement.flags&0x200u)) {
        reporting.unitFlags=movement.flags&0x1000u?reporting.unitFlags|0x80u:reporting.unitFlags&~0x80u;
    }
    if(movement.flags&0x30u)reporting.unitFlags|=0x4000000u;
    if(movement.flags&0xc0u)reporting.unitFlags|=0x8000000u;
    connection.sendMovement(packet);
    // Connection sees the old deadline. Only the active mover advances it.
    if(activeMover)clock.heartbeatTimeMs=time+500u;
    return true;
}
namespace {
bool permitsPitch(const collision::SurfaceMovementState& state) {
    return (state.flags&0x2200000u)||(state.flags2&0x20u);
}
bool unchangedAngles(const collision::SurfaceMovementState& state,const MovementReportingState& report) {
    // x87 compares the unrounded subtraction of two float properties.
    return std::abs(double(state.facing)-report.facing)<double(0.1f)&&
        (!permitsPitch(state)||std::abs(double(state.pitch)-report.pitch)<double(0.1f));
}
template<class Send>
bool flushReports(MovementReportingState& report,uint32_t time,bool timed,const Send& send) {
    bool sent=false;
    if(report.pendingTurn&&(!timed||uint32_t(time-report.turnTimeMs)>100u)) {
        sent=send(*report.pendingTurn,0,0)||sent;
        report.pendingTurn.reset();
    }
    if(report.pendingPitch&&(!timed||uint32_t(time-report.pitchTimeMs)>100u)) {
        sent=send(*report.pendingPitch,0,0)||sent;
        report.pendingPitch.reset();
    }
    return sent;
}
}
bool processLocalMovementEvent12340(
    const collision::SurfaceMovementState& state,MovementReportingState& report,
    collision::MovementClock& clock,MovementReportConnection& connection,MovementLocalEffects& effects,
    uint64_t guid,bool active,bool player,uint32_t standState,uint32_t time,
    game::Opcode opcode,bool needAck,uint32_t counter,float value) {
    using game::Opcode;
    effects.refreshMovementEffects();
    const bool turn=opcode==Opcode::MSG_MOVE_START_TURN_LEFT||opcode==Opcode::MSG_MOVE_START_TURN_RIGHT||opcode==Opcode::MSG_MOVE_STOP_TURN;
    const bool pitch=opcode==Opcode::MSG_MOVE_START_PITCH_UP||opcode==Opcode::MSG_MOVE_START_PITCH_DOWN||opcode==Opcode::MSG_MOVE_STOP_PITCH;
    const bool setAngles=opcode==Opcode::MSG_MOVE_SET_FACING||opcode==Opcode::MSG_MOVE_SET_PITCH;
    if((pitch||opcode==Opcode::MSG_MOVE_SET_PITCH)&&!permitsPitch(state))return false;
    const bool stop=opcode==Opcode::MSG_MOVE_STOP||opcode==Opcode::MSG_MOVE_STOP_STRAFE;
    const bool standingExempt=stop||turn||pitch||setAngles||hasAcknowledgementIndex(opcode)||
        opcode==Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER||opcode==Opcode::MSG_MOVE_SET_RUN_MODE||
        opcode==Opcode::MSG_MOVE_SET_WALK_MODE||opcode==Opcode::MSG_MOVE_SET_TURN_RATE_CHEAT||
        opcode==Opcode::MSG_MOVE_SET_TURN_RATE||opcode==Opcode::MSG_MOVE_TOGGLE_COLLISION_CHEAT||
        opcode==Opcode::MSG_MOVE_SET_PITCH_RATE_CHEAT||opcode==Opcode::MSG_MOVE_SET_PITCH_RATE;
    if(!standingExempt&&standState&&player)effects.standUp();
    const auto send=[&](game::Opcode message,uint32_t index,float parameter) {
        return reportUnattachedMovement12340(state,report,clock,connection,guid,active,time,message,index,parameter);
    };
    bool sent=false;
    if(needAck) {
        if((turn&&(state.flags2&8u))||(pitch&&(state.flags2&16u))) {
            auto& pending=turn?report.pendingTurn:report.pendingPitch;
            auto& started=turn?report.turnTimeMs:report.pitchTimeMs;
            const uint32_t flag=turn?0x4000000u:0x8000000u;
            const auto stopOpcode=turn?Opcode::MSG_MOVE_STOP_TURN:Opcode::MSG_MOVE_STOP_PITCH;
            if(opcode!=stopOpcode) { pending=opcode;started=time; }
            else if(pending&&!(report.unitFlags&flag)) {
                if(!unchangedAngles(state,report)) {
                    (void)send(turn?Opcode::MSG_MOVE_SET_FACING:Opcode::MSG_MOVE_SET_PITCH,0,0);
                    sent=true; // Original helper returns true even if admission rejected the send.
                }
                pending.reset();
            } else {
                (void)send(stopOpcode,0,0);
                report.unitFlags&=~flag;pending.reset();sent=true;
            }
        } else if(!setAngles||!unchangedAngles(state,report)) {
            sent=send(opcode,counter,value);
            sent=flushReports(report,time,false,send)||sent;
        }
    }
    if(active) {
        if(opcode==Opcode::CMSG_FORCE_MOVE_ROOT_ACK||opcode==Opcode::CMSG_FORCE_MOVE_UNROOT_ACK) {
            effects.updateInput(time);
            if(!(state.flags&0xc010ffu))effects.movementBecameIdle();
        } else if(stop) {
            if(!(state.flags&0xc010ffu))effects.movementBecameIdle();
        } else if(opcode==Opcode::CMSG_MOVE_SET_FLY)effects.updateInput(time);
    }
    effects.notifyMovement(opcode);
    return sent;
}
bool flushTimedMovementReports12340(
    const collision::SurfaceMovementState& state,MovementReportingState& report,
    collision::MovementClock& clock,MovementReportConnection& connection,
    uint64_t guid,bool active,uint32_t time) {
    return flushReports(report,time,true,[&](game::Opcode opcode,uint32_t counter,float value) {
        return reportUnattachedMovement12340(state,report,clock,connection,guid,active,time,opcode,counter,value);
    });
}
} // namespace wowee::core

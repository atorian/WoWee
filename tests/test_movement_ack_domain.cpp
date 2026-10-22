// Server commands through the real game; ACK values verified against build 12340.
#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "game/world_packets.hpp"

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;

TEST_CASE("forced movement flags acknowledge the original command value", "[movement][harness]") {
    struct Command { Opcode enable, disable, ack; bool hasValue; };
    const Command commands[] = {
        {Opcode::SMSG_MOVE_SET_HOVER, Opcode::SMSG_MOVE_UNSET_HOVER, Opcode::CMSG_MOVE_HOVER_ACK, true},
        {Opcode::SMSG_MOVE_FEATHER_FALL, Opcode::SMSG_MOVE_NORMAL_FALL, Opcode::CMSG_MOVE_FEATHER_FALL_ACK, true},
        {Opcode::SMSG_MOVE_WATER_WALK, Opcode::SMSG_MOVE_LAND_WALK, Opcode::CMSG_MOVE_WATER_WALK_ACK, true},
        {Opcode::SMSG_MOVE_SET_CAN_FLY, Opcode::SMSG_MOVE_UNSET_CAN_FLY, Opcode::CMSG_MOVE_SET_CAN_FLY_ACK, true},
        {Opcode::SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY, Opcode::SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,
            Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, true},
        {Opcode::SMSG_MOVE_GRAVITY_DISABLE, Opcode::SMSG_MOVE_GRAVITY_ENABLE, Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK, false}
    };
    GameHarness h;
    h.enterWorld();
    uint32_t counter = 0x10203040;
    for (const auto& command : commands) {
        for (bool enabled : {true, false}) {
            CAPTURE(static_cast<unsigned>(command.ack), enabled);
            h.clearRecords();
            h.receive(enabled ? command.enable : command.disable, Body().packedGuid(h.player().guid).u32(++counter));
            h.pump();
            const auto ackOpcode = !command.hasValue && !enabled ? Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK : command.ack;
            auto* sent = h.lastSent(ackOpcode);
            REQUIRE(sent != nullptr);
            CHECK(h.countSent(ackOpcode) == 1);
            CHECK(sent->readPackedGuid() == h.player().guid);
            CHECK(sent->readUInt32() == counter);
            wowee::game::MovementInfo status;
            REQUIRE(wowee::game::MovementPacket::readMovementPayload(*sent, status));
            CHECK(sent->getRemainingSize() == (command.hasValue ? 4 : 0));
            if (command.hasValue && sent->hasRemaining(4)) CHECK(sent->readFloat() == (enabled ? 1.0f : 0.0f));
            CHECK(sent->getRemainingSize() == 0);
        }
    }
}

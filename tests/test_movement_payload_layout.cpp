// The MSG_MOVE_* payload, read back as each expansion writes it.
//
// Another player's movement packet carries more than a position: pitch while
// swimming or flying, how long it has been in the air, and the speed and
// direction of the jump it is on. Those were read past and thrown away, so a
// jumping player was drawn with no arc. The readers now take every field the
// writers put down, and flags come back as this client's MovementFlags
// whatever the wire bits were.
//
// This is a regression net rather than an outside oracle: each reader is
// checked against its own expansion's writer.
#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/packet_parsers.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;
using wowee::network::Packet;

namespace {

constexpr uint32_t bit(MovementFlags f) { return static_cast<uint32_t>(f); }

MovementInfo jumpingForward() {
    MovementInfo info;
    info.flags = bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING);
    info.time = 123456;
    info.x = 1.5f;
    info.y = -2.25f;
    info.z = 30.0f;
    info.orientation = 0.75f;
    info.fallTime = 210;
    info.jumpVelocity = -7.955547f;
    info.jumpSinAngle = 0.6f;
    info.jumpCosAngle = 0.8f;
    info.jumpXYSpeed = 7.0f;
    return info;
}

MovementInfo readBack(PacketParsers& parsers, const MovementInfo& written) {
    Packet packet(0);
    parsers.writeMovementPayload(packet, written);
    Packet incoming(0, packet.getData());
    MovementInfo read;
    REQUIRE(parsers.readMovementPayload(incoming, read));
    CHECK(incoming.getRemainingSize() == 0);
    return read;
}

void checkJump(const MovementInfo& read, const MovementInfo& written) {
    CHECK(read.flags == written.flags);
    CHECK(read.time == written.time);
    CHECK(read.x == written.x);
    CHECK(read.y == written.y);
    CHECK(read.z == written.z);
    CHECK(read.orientation == written.orientation);
    CHECK(read.fallTime == written.fallTime);
    CHECK(read.jumpVelocity == written.jumpVelocity);
    CHECK(read.jumpSinAngle == written.jumpSinAngle);
    CHECK(read.jumpCosAngle == written.jumpCosAngle);
    CHECK(read.jumpXYSpeed == written.jumpXYSpeed);
}

}  // namespace

TEST_CASE("WotLK movement payload keeps the jump", "[movement][packet]") {
    WotlkPacketParsers parsers;
    const MovementInfo written = jumpingForward();
    checkJump(readBack(parsers, written), written);
}

TEST_CASE("WotLK movement payload keeps pitch and transport", "[movement][packet]") {
    WotlkPacketParsers parsers;
    MovementInfo written;
    written.flags = bit(MovementFlags::SWIMMING) | bit(MovementFlags::FORWARD) |
                    bit(MovementFlags::ONTRANSPORT);
    written.flags2 = 0x0400;  // interpolated movement: a second transport time
    written.pitch = -0.4f;
    written.transportGuid = 0x1F10000000000042ull;
    written.transportX = 3.0f;
    written.transportO = 1.0f;
    written.transportTime = 77;
    written.transportSeat = 2;
    written.transportTime2 = 88;

    const MovementInfo read = readBack(parsers, written);
    CHECK(read.flags == written.flags);
    CHECK(read.pitch == written.pitch);
    CHECK(read.transportGuid == written.transportGuid);
    CHECK(read.transportX == written.transportX);
    CHECK(read.transportO == written.transportO);
    CHECK(read.transportTime == written.transportTime);
    CHECK(read.transportSeat == written.transportSeat);
    CHECK(read.transportTime2 == written.transportTime2);
}

TEST_CASE("A jump's direction is cosine first on the wire", "[movement][packet]") {
    // As another player's client sends it: a jump at orientation 5.83, whose
    // first direction field is cos(5.83) = 0.90 and the second sin = -0.44.
    // Read sine first, the jump went off at right angles to where it went.
    Packet packet(0);
    packet.writeUInt32(static_cast<uint32_t>(MovementFlags::FORWARD) |
                       static_cast<uint32_t>(MovementFlags::FALLING));
    packet.writeUInt16(0);
    packet.writeUInt32(1000);
    packet.writeFloat(1.0f);
    packet.writeFloat(2.0f);
    packet.writeFloat(3.0f);
    packet.writeFloat(5.83f);
    packet.writeUInt32(0);           // fall time
    packet.writeFloat(-7.955547f);   // vertical speed
    packet.writeFloat(0.90f);        // first: cosine
    packet.writeFloat(-0.44f);       // second: sine
    packet.writeFloat(16.1f);
    Packet incoming(0, packet.getData());
    MovementInfo read;
    REQUIRE(MovementPacket::readMovementPayload(incoming, read));
    CHECK(read.jumpCosAngle == 0.90f);
    CHECK(read.jumpSinAngle == -0.44f);
    CHECK(read.jumpXYSpeed == 16.1f);
}

TEST_CASE("WotLK movement payload stops where it ends", "[movement][packet]") {
    WotlkPacketParsers parsers;
    Packet packet(0);
    parsers.writeMovementPayload(packet, jumpingForward());
    std::vector<uint8_t> bytes = packet.getData();
    bytes.resize(bytes.size() - 4);  // the jump's horizontal speed is missing
    Packet incoming(0, bytes);
    MovementInfo read;
    CHECK_FALSE(parsers.readMovementPayload(incoming, read));
}

TEST_CASE("TBC movement payload reads JUMPING as FALLING", "[movement][packet]") {
    TbcPacketParsers parsers;
    // TBC's writer takes the flag word as it goes on the wire, where being in
    // the air is JUMPING (0x2000).
    MovementInfo written = jumpingForward();
    written.flags = bit(MovementFlags::FORWARD) | 0x00002000u;
    MovementInfo expected = written;
    expected.flags = bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING);
    checkJump(readBack(parsers, written), expected);
}

TEST_CASE("Classic movement payload maps flags back", "[movement][packet]") {
    ClassicPacketParsers parsers;
    // Classic's writer maps this client's flags onto the classic wire, and
    // the reader maps them back.
    MovementInfo written = jumpingForward();
    written.flags |= bit(MovementFlags::ONTRANSPORT);
    written.transportGuid = 0x42;
    written.transportY = -5.0f;

    const MovementInfo read = readBack(parsers, written);
    checkJump(read, written);
    CHECK(read.transportGuid == written.transportGuid);
    CHECK(read.transportY == written.transportY);
}

// The MSG_MOVE_* payload, read back as each expansion writes it.
//
// Another player's movement packet carries more than a position: pitch while
// swimming or flying, how long it has been in the air, and the speed and
// direction of the jump it is on. Those were read past and thrown away, so a
// jumping player was drawn with no arc. The readers now take every field the
// writers put down, and flags come back as this client's MovementFlags
// whatever the wire bits were.
//
// WotLK's payload is laid out here byte for byte as the 3.3.5a server writes
// it. TBC's and Classic's are still checked against their own expansion's
// writer, which catches a reader that drifted from it but not an error the
// two share.
#include <catch_amalgamated.hpp>

#include "game/packet_parsers.hpp"


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
    // As 3.3.5a's MovementInfo goes on the wire: flags, flags2, time, the
    // position and facing, the fall time, then - FALLING set - the vertical
    // speed, the direction (cosine first) and the horizontal speed.
    Packet packet(0);
    packet.writeUInt32(bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING));
    packet.writeUInt16(0);
    packet.writeUInt32(123456);
    packet.writeFloat(1.5f);
    packet.writeFloat(-2.25f);
    packet.writeFloat(30.0f);
    packet.writeFloat(0.75f);
    packet.writeUInt32(210);
    packet.writeFloat(-7.955547f);
    packet.writeFloat(0.8f);
    packet.writeFloat(0.6f);
    packet.writeFloat(7.0f);

    WotlkPacketParsers parsers;
    Packet incoming(0, packet.getData());
    MovementInfo read;
    REQUIRE(parsers.readMovementPayload(incoming, read));
    CHECK(incoming.getRemainingSize() == 0);
    CHECK(read.flags == (bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING)));
    CHECK(read.time == 123456);
    CHECK(read.x == 1.5f);
    CHECK(read.y == -2.25f);
    CHECK(read.z == 30.0f);
    CHECK(read.orientation == 0.75f);
    CHECK(read.fallTime == 210);
    CHECK(read.jumpVelocity == -7.955547f);
    CHECK(read.jumpCosAngle == 0.8f);
    CHECK(read.jumpSinAngle == 0.6f);
    CHECK(read.jumpXYSpeed == 7.0f);
}

TEST_CASE("WotLK movement payload keeps pitch and transport", "[movement][packet]") {
    // ONTRANSPORT adds the transport block after the facing: packed guid, its
    // offset and facing, its time, the seat, and - interpolated movement in
    // flags2 (0x0400) - a second time. SWIMMING then adds the pitch before
    // the fall time.
    Packet packet(0);
    packet.writeUInt32(bit(MovementFlags::SWIMMING) | bit(MovementFlags::FORWARD) |
                       bit(MovementFlags::ONTRANSPORT));
    packet.writeUInt16(0x0400);
    packet.writeUInt32(1000);
    for (int i = 0; i < 4; ++i) packet.writeFloat(0.0f);  // x, y, z, facing
    packet.writePackedGuid(0x1F10000000000042ull);
    packet.writeFloat(3.0f);   // transport x
    packet.writeFloat(0.0f);
    packet.writeFloat(0.0f);
    packet.writeFloat(1.0f);   // transport facing
    packet.writeUInt32(77);
    packet.writeUInt8(2);      // seat
    packet.writeUInt32(88);    // second transport time
    packet.writeFloat(-0.4f);  // pitch
    packet.writeUInt32(0);     // fall time

    WotlkPacketParsers parsers;
    Packet incoming(0, packet.getData());
    MovementInfo read;
    REQUIRE(parsers.readMovementPayload(incoming, read));
    CHECK(incoming.getRemainingSize() == 0);
    CHECK(read.pitch == -0.4f);
    CHECK(read.transportGuid == 0x1F10000000000042ull);
    CHECK(read.transportX == 3.0f);
    CHECK(read.transportO == 1.0f);
    CHECK(read.transportTime == 77);
    CHECK(read.transportSeat == 2);
    CHECK(read.transportTime2 == 88);
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

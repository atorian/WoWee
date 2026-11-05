// The DoS proof-of-work a 3.3.5a world server asks for in SMSG_AUTH_CHALLENGE.
//
// The challenge is a difficulty, the server seed and 32 seed bytes. Wow.exe
// (build 12340, 0x632730 and 0x6321B0) hashes
//
//     SHA1(account || seed bytes || nonce as little-endian uint64)
//
// for nonce = 0, 1, 2, ... and counts zero bits from the front of the digest:
// eight for each zero byte, then the low-order zero bits of the first byte that
// is not zero. The first nonce with at least `difficulty` of them goes out in
// CMSG_AUTH_SESSION as the "DoS response". The client sent a constant zero
// there, which a difficulty of 1 accepts only when the first digest byte
// happens to be even.
//
// The expected nonces were computed with Python's hashlib from that
// description, not with this code.
#include <catch_amalgamated.hpp>

#include "game/world_packets.hpp"
#include "client/application.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

std::array<uint8_t, 32> ascendingSeeds() {
    std::array<uint8_t, 32> seeds{};
    for (size_t i = 0; i < seeds.size(); ++i) seeds[i] = static_cast<uint8_t>(i);
    return seeds;
}

uint32_t readU32(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint64_t readU64(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint64_t>(readU32(b, at)) |
           (static_cast<uint64_t>(readU32(b, at + 4)) << 32);
}

} // namespace

TEST_CASE("The DoS response is the first nonce Wow.exe would accept",
          "[auth][packet]") {
    const auto seeds = ascendingSeeds();

    SECTION("difficulty 0 accepts the first nonce") {
        CHECK(AuthSessionPacket::solveDosChallenge("PLAYER", seeds, 0) == 0u);
    }

    SECTION("difficulty 1 rejects nonce 0, whose digest starts with an odd byte") {
        CHECK(AuthSessionPacket::solveDosChallenge("PLAYER", seeds, 1) == 1u);
    }

    SECTION("difficulty 12 needs a whole zero byte and four low zero bits") {
        CHECK(AuthSessionPacket::solveDosChallenge("PLAYER", seeds, 12) == 4653u);
    }

    SECTION("the account is hashed in upper case, as the session digest is") {
        CHECK(AuthSessionPacket::solveDosChallenge("player", seeds, 12) == 4653u);
    }

    SECTION("more zero bits than SHA1 has cannot be met") {
        CHECK_FALSE(AuthSessionPacket::solveDosChallenge("PLAYER", seeds, 161).has_value());
    }
}

TEST_CASE("SMSG_AUTH_CHALLENGE keeps the difficulty and the seed bytes",
          "[auth][packet]") {
    wowee::network::Packet packet(0x1EC);
    packet.writeUInt32(1);            // difficulty
    packet.writeUInt32(0xAABBCCDD);   // server seed
    const auto seeds = ascendingSeeds();
    packet.writeBytes(seeds.data(), seeds.size());

    AuthChallengeData challenge;
    REQUIRE(AuthChallengeParser::parse(packet, challenge));
    CHECK(challenge.dosDifficulty == 1u);
    CHECK(challenge.serverSeed == 0xAABBCCDDu);
    CHECK(challenge.dosSeeds == seeds);
}

TEST_CASE("CMSG_AUTH_SESSION carries the DoS response after the realm id",
          "[auth][packet]") {
    const std::vector<uint8_t> sessionKey(40, 0x11);
    auto packet = AuthSessionPacket::build(12340, "player", 0x01020304, sessionKey,
                                           0xAABBCCDD, 9, 4653);
    const auto& b = packet.getData();

    // build(4) LoginServerID(4) "PLAYER\0"(7) LoginServerType(4)
    // LocalChallenge(4) RegionID(4) BattlegroupID(4) RealmID(4) DosResponse(8)
    REQUIRE(b.size() >= 43);
    CHECK(readU32(b, 31) == 9u);
    CHECK(readU64(b, 35) == 4653u);
}

// The addon block of CMSG_AUTH_SESSION, as Wow.exe 12340 writes it (0x5F5440):
// a uint32 uncompressed size, then a zlib stream of
//
//     uint32 count, { string name, uint8 hasKey, uint32 crc32(key),
//                     uint32 crc32(url) } * count, uint32 time
//
// for the addons whose TOC says "## Secure: 1" - the 23 Blizzard_* addons. The
// key comes from Interface\AddOns\<name>\<name>.pub, which the client writes
// when SMSG_ADDON_INFO hands one over; the original Isengard client on this
// machine holds the standard Blizzard key (CRC32 0x4C1C776D) for all 23 and no
// .url files.
TEST_CASE("CMSG_AUTH_SESSION lists the secure Blizzard addons with their key CRC",
          "[auth][packet]") {
    const std::vector<uint8_t> sessionKey(40, 0x11);
    auto packet = AuthSessionPacket::build(12340, "player", 0x01020304, sessionKey,
                                           0xAABBCCDD, 9, 0);
    const auto& b = packet.getData();

    // 35 bytes of header fields, the 8-byte DoS response, the 20-byte digest.
    const size_t addonAt = 35 + 8 + 20;
    REQUIRE(b.size() > addonAt + 4);
    const uint32_t declaredSize = readU32(b, addonAt);

    std::vector<uint8_t> addons(declaredSize);
    uLongf actualSize = declaredSize;
    REQUIRE(uncompress(addons.data(), &actualSize, b.data() + addonAt + 4,
                       static_cast<uLong>(b.size() - addonAt - 4)) == Z_OK);
    REQUIRE(actualSize == declaredSize);

    size_t at = 0;
    auto readName = [&] {
        std::string name;
        while (at < addons.size() && addons[at] != 0) name.push_back(static_cast<char>(addons[at++]));
        ++at;
        return name;
    };

    REQUIRE(readU32(addons, at) == 23u);
    at += 4;
    std::vector<std::string> names;
    for (int i = 0; i < 23; ++i) {
        names.push_back(readName());
        INFO(names.back());
        CHECK(addons[at] == 1);                        // hasKey
        CHECK(readU32(addons, at + 1) == 0x4C1C776Du); // CRC32 of the key
        CHECK(readU32(addons, at + 5) == 0u);          // no .url
        at += 9;
    }
    CHECK(readU32(addons, at) == 0u);                  // banned-list time
    CHECK(at + 4 == addons.size());

    CHECK(names.front() == "Blizzard_AchievementUI");
    CHECK(names[3] == "Blizzard_BarbershopUI");
    CHECK(names.back() == "Blizzard_TrainerUI");
}

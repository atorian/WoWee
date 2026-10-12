#include <catch_amalgamated.hpp>
#include "game/warden_known_modules.hpp"
#include "game/warden_crypto.hpp"
#include "game/warden_constants.hpp"
#include "auth/crypto.hpp"
#include <numeric>
#include <vector>

using namespace wowee::game;

TEST_CASE("Built-in Warden reply is limited to the exact module, key and seed", "[warden]") {
    const auto& m = kStandardWardenChallenge;
    REQUIRE(knownWardenChallenge(m.moduleHash, m.moduleKey, m.seed) != nullptr);
    auto hash = m.moduleHash;
    hash[0] ^= 1;
    CHECK(knownWardenChallenge(hash, m.moduleKey, m.seed) == nullptr);
    auto key = m.moduleKey;
    key[0] ^= 1;
    CHECK(knownWardenChallenge(m.moduleHash, key, m.seed) == nullptr);
    auto seed = m.seed;
    seed[0] ^= 1;
    CHECK(knownWardenChallenge(m.moduleHash, m.moduleKey, seed) == nullptr);
    CHECK(knownWardenChallenge({}, m.moduleKey, m.seed) == nullptr);
    CHECK(knownWardenChallenge(m.moduleHash, {}, m.seed) == nullptr);
    CHECK(knownWardenChallenge(m.moduleHash, m.moduleKey, {}) == nullptr);
    CHECK(knownWardenChallenge(m.moduleHash, m.moduleKey,
                              std::span<const uint8_t>(m.seed).first(15)) == nullptr);
}

TEST_CASE("Standard Warden hash authenticates the client key, not the downloaded image", "[warden]") {
    const auto& m = kStandardWardenChallenge;
    auto hash = wowee::auth::Crypto::sha1(std::vector<uint8_t>(m.clientKey.begin(), m.clientKey.end()));
    CHECK(std::ranges::equal(hash, m.reply));
    CHECK(m.checkOpcodes[0] == 0xF3); // server MEM_CHECK
    CHECK(m.checkOpcodes[8] == 0x57); // server TIMING_CHECK
}

TEST_CASE("Standard Warden handshake sends hash before rekeying both streams", "[warden]") {
    WardenCrypto crypto;
    std::vector<uint8_t> session(40);
    std::iota(session.begin(), session.end(), 0);
    REQUIRE(crypto.initFromSessionKey(session));
    (void)crypto.encrypt({WARDEN_CMSG_MODULE_MISSING});
    (void)crypto.encrypt({WARDEN_CMSG_MODULE_OK});
    const auto& m = kStandardWardenChallenge;
    std::vector<uint8_t> reply = {WARDEN_CMSG_HASH_RESULT};
    reply.insert(reply.end(), m.reply.begin(), m.reply.end());
    // Independent RC4/SHA1 vectors include the two preceding module responses.
    CHECK(crypto.encrypt(reply) == std::vector<uint8_t> {0xE3,0x83,0xA7,0x4D,0x75,0x93,0xBC,0xB8,0x34,0x1C,0xA1,0xA1,0x38,0x11,0x3A,0x1B,0x24,0xD6,0x19,0xFB,0x95});
    crypto.replaceKeys(std::vector<uint8_t>(m.clientKey.begin(), m.clientKey.end()),
                       std::vector<uint8_t>(m.serverKey.begin(), m.serverKey.end()));
    // The first server message after HASH_RESULT uses the NEW server key.
    CHECK(crypto.decrypt({0x51,0x62,0xC7,0x88,0x1D,0x0E,0x07,0x97,0x50,0x43,0x75,0x27,0x77,0x09,0x9B}) == std::vector<uint8_t> {0x03,0x08,0x00,0x11,0x22,0x33,0x44,0x04,0x00,0x00,0x10,0x92,0x41,0x00,0x01});
    // The next check reply uses the NEW client key, without carrying old RC4 state.
    CHECK(crypto.encrypt({0x02,0x05,0x00,0x11,0x22,0x33,0x44,0x01,0x78,0x56,0x34,0x12}) == std::vector<uint8_t> {0xCA,0xBC,0x21,0x60,0xD9,0xCC,0xB8,0x1D,0x9F,0xA5,0xDA,0xDA});
}


TEST_CASE("Cached Warden handshake advances RC4 only for MODULE_OK before hash", "[warden]") {
    WardenCrypto crypto;
    std::vector<uint8_t> session(40);
    std::iota(session.begin(), session.end(), 0);
    REQUIRE(crypto.initFromSessionKey(session));
    (void)crypto.encrypt({WARDEN_CMSG_MODULE_OK});
    const auto& module = kStandardWardenChallenge;
    std::vector<uint8_t> reply{WARDEN_CMSG_HASH_RESULT};
    reply.insert(reply.end(), module.reply.begin(), module.reply.end());
    // Independent SHA1Randx/RC4 vector with no MODULE_MISSING byte.
    CHECK(crypto.encrypt(reply) == std::vector<uint8_t>{0x85,0xB1,0x59,0x2E,0x04,0x41,0xF1,0x31,0x05,0x7E,0x4B,0x34,0x2A,0x1D,0x3F,0xAD,0x8B,0x70,0x07,0xD6,0xDA});
    crypto.replaceKeys(std::vector<uint8_t>(module.clientKey.begin(), module.clientKey.end()),
                       std::vector<uint8_t>(module.serverKey.begin(), module.serverKey.end()));
    CHECK(crypto.encrypt({0x02,0x05,0x00,0x11,0x22,0x33,0x44,0x01,0x78,0x56,0x34,0x12}) ==
          std::vector<uint8_t>{0xCA,0xBC,0x21,0x60,0xD9,0xCC,0xB8,0x1D,0x9F,0xA5,0xDA,0xDA});
}


#include "game/warden_module_cache.hpp"
#include <fstream>

TEST_CASE("Warden cache validates contents and bounds before reuse", "[warden]") {
    namespace fs = std::filesystem;
    const std::vector<uint8_t> bytes{1, 2, 3, 4, 5};
    const auto hash = wowee::auth::Crypto::md5(bytes);
    const auto directory = fs::temp_directory_path() / ("wowee_warden_cache_" + std::to_string(reinterpret_cast<uintptr_t>(&bytes)));
    fs::create_directories(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    const auto file = directory / "module.wdn";
    CHECK(readVerifiedWardenModuleCache(file, hash, bytes.size()).empty());
    { std::ofstream out(file, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    CHECK(readVerifiedWardenModuleCache(file, hash, bytes.size()) == bytes);
    CHECK(readVerifiedWardenModuleCache(file, hash, bytes.size() - 1).empty());
    CHECK(readVerifiedWardenModuleCache(file, hash, bytes.size() + 1).empty());
    CHECK(readVerifiedWardenModuleCache(file, hash, 0).empty());
    CHECK(readVerifiedWardenModuleCache(file, hash, 1024 * 1024 + 1).empty());
    CHECK(readVerifiedWardenModuleCache(file, {}, bytes.size()).empty());
    auto changedHash = hash; changedHash[0] ^= 1;
    CHECK(readVerifiedWardenModuleCache(file, changedHash, bytes.size()).empty());
    { std::ofstream out(file, std::ios::binary); out.write("wrong", 5); }
    CHECK(readVerifiedWardenModuleCache(file, hash, bytes.size()).empty());
}

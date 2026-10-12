/// The keyed-once HMAC-SHA1 behind Warden's code-pattern search must agree
/// with OpenSSL byte for byte: a wrong digest answers "not found" for a
/// pattern that is there, which the server reads as a modified client.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "game/warden_hmac.hpp"

using wowee::game::WardenHmacSha1;

TEST_CASE("keyed-once HMAC-SHA1 matches OpenSSL at every message length", "[warden]") {
    std::mt19937 rng(12340);
    std::vector<uint8_t> data(4096);
    for (auto& b : data) b = static_cast<uint8_t>(rng());

    for (int trial = 0; trial < 8; ++trial) {
        uint8_t seed[4];
        for (auto& b : seed) b = static_cast<uint8_t>(rng());
        WardenHmacSha1 hmac(seed, sizeof(seed));
        // Every length a check can name, so each SHA1 padding boundary
        // (55/56, 119/120, ...) is crossed.
        for (size_t len = 1; len <= 255; ++len) {
            const size_t at = rng() % (data.size() - len);
            uint8_t expected[20];
            unsigned size = 0;
            HMAC(EVP_sha1(), seed, 4, data.data() + at, len, expected, &size);
            REQUIRE(size == 20);
            uint8_t got[20];
            REQUIRE(hmac.digest(data.data() + at, len, got));
            INFO("trial " << trial << " length " << len);
            REQUIRE(std::equal(got, got + 20, expected));
        }
    }
}

TEST_CASE("a key longer than the seed is padded the same way", "[warden]") {
    const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t msg[] = "WoW.exe";
    uint8_t expected[20];
    unsigned size = 0;
    HMAC(EVP_sha1(), key, sizeof(key), msg, 7, expected, &size);
    uint8_t got[20];
    WardenHmacSha1 hmac(key, sizeof(key));
    REQUIRE(hmac.digest(msg, 7, got));
    CHECK(std::equal(got, got + 20, expected));
}

// Hidden: `test_warden_hmac [.bench]`. One thread over as many positions as a
// full WoW.exe search, against the one-shot HMAC() the search used before.
TEST_CASE("keyed-once HMAC-SHA1 throughput", "[.bench]") {
    std::vector<uint8_t> image(10473460 + 64);
    std::mt19937 rng(1);
    for (auto& b : image) b = static_cast<uint8_t>(rng());
    const uint8_t seed[4] = {9, 8, 7, 6};
    const size_t positions = image.size() - 64;
    uint8_t out[20];
    unsigned sink = 0;

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < positions; ++i) {
        unsigned size = 0;
        HMAC(EVP_sha1(), seed, 4, image.data() + i, 13, out, &size);
        sink += out[0];
    }
    auto t1 = std::chrono::steady_clock::now();
    WardenHmacSha1 hmac(seed, 4);
    for (size_t i = 0; i < positions; ++i) {
        hmac.digest(image.data() + i, 13, out);
        sink += out[0];
    }
    auto t2 = std::chrono::steady_clock::now();
    const double before = std::chrono::duration<double>(t1 - t0).count();
    const double after = std::chrono::duration<double>(t2 - t1).count();
    WARN("one-shot HMAC: " << before << " s, keyed once: " << after << " s, x" << before / after
         << " (sink " << sink << ")");
}


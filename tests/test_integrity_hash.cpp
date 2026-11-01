// The LOGON_PROOF integrity hash of a 3.3.5a (12340) Windows client.
//
// Wow.exe (0x6B9CF0, OsVersionHash.cpp) keys HMAC-SHA1 with the 16-byte salt
// from the logon challenge and feeds it Wow.exe and DivxDecoder.dll as loaded
// in memory, then unicows.dll as a file. "As loaded" is what the loader maps:
// the headers (e_csum cleared) and every section, each padded with zeros to
// the section alignment, writable sections as zeros. The proof field is
// SHA1(A || checksum).
//
// The client hashed only the raw Wow.exe file, which gives a different value.
// TrinityCore sends a fixed salt (BAA31E99...) and keeps the checksum a real
// 12340 Windows client produces for it in build_executable_hash:
// CDCBBD5188315E6B4D19449D492DBCFAF156A347. Isengard sends the same salt.
#include <catch_amalgamated.hpp>

#include "auth/integrity.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace wowee::auth;

namespace {

void putU16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void putU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// A PE32 file with a 0x200-byte header, a read-only section whose 0x200 raw
// bytes are only 0x10 long in memory, and a writable section.
std::vector<uint8_t> twoSectionPe() {
    std::vector<uint8_t> pe(0x600, 0);
    pe[0] = 'M'; pe[1] = 'Z';
    putU16(pe, 0x12, 0xBEEF);                 // e_csum
    putU32(pe, 0x3C, 0x40);                   // e_lfanew
    pe[0x40] = 'P'; pe[0x41] = 'E';
    putU16(pe, 0x46, 2);                      // NumberOfSections
    putU16(pe, 0x54, 0xE0);                   // SizeOfOptionalHeader
    const size_t opt = 0x58;
    putU16(pe, opt, 0x10B);                   // PE32
    putU32(pe, opt + 0x20, 0x1000);           // SectionAlignment
    putU32(pe, opt + 0x24, 0x200);            // FileAlignment
    putU32(pe, opt + 0x3C, 0x200);            // SizeOfHeaders
    const size_t sec = opt + 0xE0;
    // .text: VirtualSize 0x10, VA 0x1000, raw 0x200 at 0x200, code
    putU32(pe, sec + 8, 0x10);
    putU32(pe, sec + 12, 0x1000);
    putU32(pe, sec + 16, 0x200);
    putU32(pe, sec + 20, 0x200);
    putU32(pe, sec + 36, 0x60000020);
    // .data: VirtualSize 0x30, VA 0x2000, raw 0x200 at 0x400, writable
    putU32(pe, sec + 40 + 8, 0x30);
    putU32(pe, sec + 40 + 12, 0x2000);
    putU32(pe, sec + 40 + 16, 0x200);
    putU32(pe, sec + 40 + 20, 0x400);
    putU32(pe, sec + 40 + 36, 0xC0000040);
    for (size_t i = 0x200; i < 0x400; ++i) pe[i] = 0xAA;
    for (size_t i = 0x400; i < 0x600; ++i) pe[i] = 0xBB;
    return pe;
}

} // namespace

TEST_CASE("The integrity image is the PE as the loader maps it", "[auth][integrity]") {
    const auto pe = twoSectionPe();
    std::vector<uint8_t> image;
    std::string err;
    REQUIRE(loadedImageForIntegrity(pe, image, err));

    // Header page, .text page, .data page.
    REQUIRE(image.size() == 0x3000);

    std::vector<uint8_t> header(pe.begin(), pe.begin() + 0x200);
    header[0x12] = 0;
    header[0x13] = 0;
    CHECK(std::equal(header.begin(), header.end(), image.begin()));
    CHECK(std::all_of(image.begin() + 0x200, image.begin() + 0x1000,
                      [](uint8_t b) { return b == 0; }));

    CHECK(std::all_of(image.begin() + 0x1000, image.begin() + 0x1010,
                      [](uint8_t b) { return b == 0xAA; }));
    CHECK(std::all_of(image.begin() + 0x1010, image.begin() + 0x2000,
                      [](uint8_t b) { return b == 0; }));

    CHECK(std::all_of(image.begin() + 0x2000, image.end(),
                      [](uint8_t b) { return b == 0; }));
}

TEST_CASE("A file that is not a PE image is refused", "[auth][integrity]") {
    auto pe = twoSectionPe();
    std::vector<uint8_t> image;
    std::string err;

    SECTION("truncated inside the section table") {
        pe.resize(0x150);
        CHECK_FALSE(loadedImageForIntegrity(pe, image, err));
    }
    SECTION("no PE signature") {
        pe[0x40] = 'X';
        CHECK_FALSE(loadedImageForIntegrity(pe, image, err));
    }
    SECTION("section data past the end of the file") {
        putU32(pe, 0x58 + 0xE0 + 20, 0x700);
        CHECK_FALSE(loadedImageForIntegrity(pe, image, err));
    }
}

TEST_CASE("A real 12340 client gives TrinityCore's Windows checksum", "[auth][integrity]") {
    const char* dir = std::getenv("WOWEE_TEST_CLIENT_DIR");
    if (!dir) { SKIP("Set WOWEE_TEST_CLIENT_DIR to a 3.3.5a (12340) Windows client folder"); }

    const std::array<uint8_t, 16> versionChallenge = {
        0xBA, 0xA3, 0x1E, 0x99, 0xA0, 0x0B, 0x21, 0x57,
        0xFC, 0x37, 0x3F, 0xB3, 0x69, 0xCD, 0xD2, 0xF1};
    const std::vector<uint8_t> a(32, 0x01);
    std::array<uint8_t, 20> proof{};
    std::string err;
    REQUIRE(computeIntegrityHashWin32WithExe(versionChallenge, a, dir, "Wow.exe", 12340, proof, err));

    // SHA1(32 x 0x01 || CDCBBD5188315E6B4D19449D492DBCFAF156A347)
    const std::array<uint8_t, 20> expected = {
        0x1e, 0x04, 0xc2, 0x0b, 0xfa, 0x9a, 0x4f, 0xfd, 0xf5, 0xf2,
        0x2c, 0x77, 0x14, 0x90, 0x80, 0xee, 0x48, 0x53, 0xe6, 0xd9};
    CHECK(proof == expected);
}

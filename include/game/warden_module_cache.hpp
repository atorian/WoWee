#pragma once
#include "auth/crypto.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace wowee::game {
// The cache contains bytes previously received from the server. Never accept
// the filename alone as verification or allocate from an unbounded wire size.
inline std::vector<uint8_t> readVerifiedWardenModuleCache(
    const std::filesystem::path& path, std::span<const uint8_t> expectedHash,
    uint32_t expectedSize) {
    constexpr uint32_t maxSize = 1024 * 1024;
    if (expectedHash.size() != 16 || !expectedSize || expectedSize > maxSize) return {};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != std::streamoff(expectedSize)) return {};
    file.seekg(0);
    std::vector<uint8_t> bytes(expectedSize);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), expectedSize) ||
        file.peek() != std::char_traits<char>::eof()) return {};
    if (!std::ranges::equal(auth::Crypto::md5(bytes), expectedHash)) return {};
    return bytes;
}
}

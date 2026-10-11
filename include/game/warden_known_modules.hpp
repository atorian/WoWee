#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace wowee::game {

struct WardenModuleChallenge {
    std::array<uint8_t, 16> moduleHash;
    std::array<uint8_t, 16> moduleKey;
    std::array<uint8_t, 16> seed;
    std::array<uint8_t, 20> reply;
    std::array<uint8_t, 16> clientKey;
    std::array<uint8_t, 16> serverKey;
    // CMaNGOS .cr order: MEM, MODULE, PAGE_A, PAGE_B, MPQ, LUA, PROC, DRIVER, TIMING.
    std::array<uint8_t, 9> checkOpcodes;
};

// Standard Windows module shared by AzerothCore, TrinityCore and MaNGOS.
// Protocol values: AzerothCore src/server/game/Warden/Modules/WardenModuleWin.h
// Check opcodes: AzerothCore src/server/game/Warden/Warden.h
// https://github.com/azerothcore/azerothcore-wotlk/tree/master/src/server/game/Warden
// These are module-specific protocol constants, not session credentials.
inline constexpr WardenModuleChallenge kStandardWardenChallenge = {
    {0x79,0xC0,0x76,0x8D,0x65,0x79,0x77,0xD6,0x97,0xE1,0x0B,0xAD,0x95,0x6C,0xCE,0xD1},
    {0xAE,0x25,0xBC,0x51,0x06,0x3B,0x77,0xBD,0x36,0x3C,0x3E,0xFE,0x0F,0xC1,0x73,0xF9},
    {0x4D,0x80,0x8D,0x2C,0x77,0xD9,0x05,0xC4,0x1A,0x63,0x80,0xEC,0x08,0x58,0x6A,0xFE},
    {0x56,0x8C,0x05,0x4C,0x78,0x1A,0x97,0x2A,0x60,0x37,0xA2,0x29,0x0C,0x22,0xB5,0x25,0x71,0xA0,0x6F,0x4E},
    {0x7F,0x96,0xEE,0xFD,0xA5,0xB6,0x3D,0x20,0xA4,0xDF,0x8E,0x00,0xCB,0xF4,0x83,0x04},
    {0xC2,0xB7,0xAD,0xED,0xFC,0xCC,0xA9,0xC2,0xBF,0xB3,0xF8,0x56,0x02,0xBA,0x80,0x9B},
    {0xF3,0xD9,0xB2,0xBF,0x98,0x8B,0x7E,0x71,0x57}
};

inline const WardenModuleChallenge* knownWardenModule(std::span<const uint8_t> moduleHash,
                                                     std::span<const uint8_t> moduleKey) {
    const auto& module = kStandardWardenChallenge;
    if (!std::ranges::equal(moduleHash, module.moduleHash) ||
        !std::ranges::equal(moduleKey, module.moduleKey)) return nullptr;
    return &module;
}

inline const WardenModuleChallenge* knownWardenChallenge(std::span<const uint8_t> moduleHash,
                                                        std::span<const uint8_t> moduleKey,
                                                        std::span<const uint8_t> seed) {
    const auto* module = knownWardenModule(moduleHash, moduleKey);
    return module && std::ranges::equal(seed, module->seed) ? module : nullptr;
}

} // namespace wowee::game

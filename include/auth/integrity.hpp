#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace auth {

// Computes the LOGON_PROOF "CRC hash" / integrity hash for the legacy WoW login protocol.
//
// Algorithm (per WoWDev/gtker docs):
//   checksum = HMAC_SHA1(checksumSalt, concatenated_file_bytes)
//   crc_hash = SHA1(clientPublicKey || checksum)
//
// clientPublicKey is the 32-byte A as sent on the wire.
//
// Returns false if any file is missing/unreadable.
bool computeIntegrityHashWin32(const std::array<uint8_t, 16>& checksumSalt,
                               const std::vector<uint8_t>& clientPublicKeyA,
                               const std::string& miscDir,
                               std::array<uint8_t, 20>& outHash,
                               std::string& outError);

// Same as computeIntegrityHashWin32, but allows selecting the EXE filename used in the file set.
// clientBuild controls the file set: Classic (<=6005) hashes the files of the
// exe, fmod, ijl15, dbghelp and unicows; 3.3.5a (12340) hashes the exe and
// DivxDecoder.dll as loaded (loadedImageForIntegrity), then unicows.dll as a
// file; other builds hash the exe file alone.
//
// DivxDecoder.dll is looked for beside the exe and then one folder up, where
// the extractor leaves it.
bool computeIntegrityHashWin32WithExe(const std::array<uint8_t, 16>& checksumSalt,
                                      const std::vector<uint8_t>& clientPublicKeyA,
                                      const std::string& miscDir,
                                      const std::string& exeName,
                                      uint16_t clientBuild,
                                      std::array<uint8_t, 20>& outHash,
                                      std::string& outError);

// A PE file as Wow.exe 12340 hashes a loaded module (OsVersionHash.cpp,
// 0x871D20): the headers with e_csum cleared, then each section, each padded
// with zeros to the section alignment; writable sections contribute zeros.
// Returns false for a file that is not a well-formed PE image.
bool loadedImageForIntegrity(const std::vector<uint8_t>& peFile,
                             std::vector<uint8_t>& outImage,
                             std::string& outError);

} // namespace auth
} // namespace wowee

#include "auth/integrity.hpp"
#include "auth/crypto.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace wowee {
namespace auth {

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "missing: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) size = 0;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) {
            err = "read failed: " + path;
            return false;
        }
    }
    return true;
}

namespace {

bool readU16(const std::vector<uint8_t>& b, size_t at, uint16_t& out) {
    if (at + 2 > b.size()) return false;
    out = static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
    return true;
}

bool readU32(const std::vector<uint8_t>& b, size_t at, uint32_t& out) {
    if (at + 4 > b.size()) return false;
    out = static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
          (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
    return true;
}

size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

bool loadedImageForIntegrity(const std::vector<uint8_t>& pe,
                             std::vector<uint8_t>& out,
                             std::string& err) {
    constexpr uint32_t kScnMemWrite = 0x80000000;
    uint32_t peAt = 0;
    uint16_t sectionCount = 0, optionalSize = 0;
    if (pe.size() < 0x40 || pe[0] != 'M' || pe[1] != 'Z' || !readU32(pe, 0x3C, peAt) ||
        peAt + 0x18 > pe.size() || pe[peAt] != 'P' || pe[peAt + 1] != 'E' ||
        pe[peAt + 2] != 0 || pe[peAt + 3] != 0 ||
        !readU16(pe, peAt + 6, sectionCount) || !readU16(pe, peAt + 0x14, optionalSize)) {
        err = "not a PE image";
        return false;
    }
    const size_t optional = peAt + 0x18;
    uint32_t sectionAlignment = 0, headerSize = 0;
    if (!readU32(pe, optional + 0x20, sectionAlignment) || !readU32(pe, optional + 0x3C, headerSize) ||
        sectionAlignment == 0 || headerSize > pe.size()) {
        err = "bad PE optional header";
        return false;
    }

    out.assign(pe.begin(), pe.begin() + headerSize);
    if (out.size() >= 0x14) {
        out[0x12] = 0;  // e_csum
        out[0x13] = 0;
    }
    out.resize(alignUp(headerSize, sectionAlignment), 0);

    const size_t table = optional + optionalSize;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t header = table + static_cast<size_t>(i) * 40;
        uint32_t virtualSize = 0, rawSize = 0, rawAt = 0, characteristics = 0;
        if (!readU32(pe, header + 8, virtualSize) || !readU32(pe, header + 16, rawSize) ||
            !readU32(pe, header + 20, rawAt) || !readU32(pe, header + 36, characteristics)) {
            err = "truncated section table";
            return false;
        }
        const size_t mapped = alignUp(virtualSize, sectionAlignment);
        const size_t start = out.size();
        out.resize(start + mapped, 0);
        if (characteristics & kScnMemWrite) continue;
        const size_t copied = std::min(virtualSize, rawSize);
        if (static_cast<size_t>(rawAt) + copied > pe.size()) {
            err = "section data past end of file";
            return false;
        }
        std::copy(pe.begin() + rawAt, pe.begin() + rawAt + copied, out.begin() + start);
    }
    return true;
}

bool computeIntegrityHashWin32WithExe(const std::array<uint8_t, 16>& checksumSalt,
                                      const std::vector<uint8_t>& clientPublicKeyA,
                                      const std::string& miscDir,
                                      const std::string& exeName,
                                      uint16_t clientBuild,
                                      std::array<uint8_t, 20>& outHash,
                                      std::string& outError) {
    // Classic 1.12.x (build <=6005) ships fmod.dll, ijl15.dll, dbghelp.dll,
    // unicows.dll alongside WoW.exe.  TBC/WotLK clients do not; hashing
    // only the .exe is sufficient (servers rarely validate the CRC for those).
    const bool isClassicBuild = clientBuild <= 6005;
    // 3.3.5a hashes two modules as they sit in memory (Wow.exe 0x6B9CF0); the
    // result matches TrinityCore's build_executable_hash for 12340 'Win'.
    const bool isWotlk12340 = (clientBuild == 12340);
    // Some macOS client layouts use FMOD dylib naming instead of fmod.dll.
    // We accept the first matching filename in each alias group.
    struct FileGroup {
        std::vector<std::string> names;
        bool asLoadedImage = false;
    };
    std::vector<FileGroup> fileGroups = {
        { { exeName }, isWotlk12340 },
    };
    if (isWotlk12340) {
        fileGroups.push_back({ { "DivxDecoder.dll", "divxdecoder.dll",
                                 "../DivxDecoder.dll", "../divxdecoder.dll" }, true });
        fileGroups.push_back({ { "unicows.dll" } });
    }
    if (isClassicBuild) {
        fileGroups.push_back({ { "fmod.dll", "fmod.dylib", "libfmod.dylib", "fmodex.dll", "fmodex.dylib", "libfmod.so" } });
        fileGroups.push_back({ { "ijl15.dll" } });
        fileGroups.push_back({ { "dbghelp.dll" } });
        fileGroups.push_back({ { "unicows.dll" } });
    }

    std::vector<uint8_t> allFiles;
    for (const auto& fileGroup : fileGroups) {
        const auto& group = fileGroup.names;
        bool foundInGroup = false;
        std::string groupErr;

        for (const auto& nameStr : group) {
            std::vector<uint8_t> bytes;
            std::string path = miscDir;
            if (!path.empty() && path.back() != '/') path += '/';
            path += nameStr;

            std::string err;
            if (!readWholeFile(path, bytes, err)) {
                if (groupErr.empty()) groupErr = err;
                continue;
            }

            if (fileGroup.asLoadedImage) {
                std::vector<uint8_t> image;
                if (!loadedImageForIntegrity(bytes, image, err)) {
                    outError = path + ": " + err;
                    return false;
                }
                bytes = std::move(image);
            }
            allFiles.insert(allFiles.end(), bytes.begin(), bytes.end());
            foundInGroup = true;
            break;
        }

        if (!foundInGroup) {
            outError = groupErr.empty() ? "missing required integrity file group" : groupErr;
            return false;
        }
    }

    // HMAC_SHA1(checksumSalt, allFiles)
    std::vector<uint8_t> key(checksumSalt.begin(), checksumSalt.end());
    const std::vector<uint8_t> checksum = Crypto::hmacSHA1(key, allFiles); // 20 bytes

    // SHA1(A || checksum)
    std::vector<uint8_t> shaIn;
    shaIn.reserve(clientPublicKeyA.size() + checksum.size());
    shaIn.insert(shaIn.end(), clientPublicKeyA.begin(), clientPublicKeyA.end());
    shaIn.insert(shaIn.end(), checksum.begin(), checksum.end());
    const std::vector<uint8_t> finalHash = Crypto::sha1(shaIn);

    if (finalHash.size() != outHash.size()) {
        outError = "unexpected sha1 size";
        return false;
    }
    std::copy(finalHash.begin(), finalHash.end(), outHash.begin());
    return true;
}

bool computeIntegrityHashWin32(const std::array<uint8_t, 16>& checksumSalt,
                               const std::vector<uint8_t>& clientPublicKeyA,
                               const std::string& miscDir,
                               std::array<uint8_t, 20>& outHash,
                               std::string& outError) {
    return computeIntegrityHashWin32WithExe(checksumSalt, clientPublicKeyA, miscDir, "WoW.exe", 5875, outHash, outError);
}

} // namespace auth
} // namespace wowee

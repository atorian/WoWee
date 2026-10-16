#pragma once

/// The handful of things a test that reads a file out of the tree needs.
///
/// Each of these was written out in two test files, and they are the shape
/// that drifts quietly: a test whose helper stops finding a file does not
/// fail, it reads an empty buffer and asserts nothing about it, and a green
/// run means the file was never opened rather than that its contents were
/// right. `dbcPathFor` is the one where that matters most - it matches the
/// table name case-insensitively because the extracted archives disagree
/// about capitalisation, so a copy that lost the fold would find nothing on
/// one machine and everything on another.
///
/// WOWEE_SOURCE_DIR is a compile definition the test's CMake block sets. Not
/// every test target sets it, so the fallback is an empty root and the caller
/// gets whatever a relative path means - which is what each copy already did.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace wowee::test {

/// The tree's root with a trailing slash, or empty when nothing defined it.
inline const std::string& sourceRoot() {
#ifdef WOWEE_SOURCE_DIR
    static const std::string root = std::string(WOWEE_SOURCE_DIR) + "/";
#else
    static const std::string root;
#endif
    return root;
}

/// A whole text file, or an empty string when it is not there.
inline std::string slurp(const std::string& relativePath) {
    std::ifstream in(sourceRoot() + relativePath);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// A whole binary file, or an empty vector when it is not there.
inline std::vector<uint8_t> readFile(const std::string& relativePath) {
    std::ifstream in(sourceRoot() + relativePath, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

/// A little-endian uint32 at `offset`.
///
/// Fails the test on a short buffer rather than answering zero, which is what
/// both copies did: this reads a packet the test has just built, so a buffer
/// too short to hold the field means the builder is wrong, and a zero there
/// would be compared against an expected value and reported as the wrong
/// number rather than as a missing one.
///
/// Include this header *after* catch_amalgamated.hpp - REQUIRE comes from it.
inline uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
    REQUIRE(bytes.size() >= offset + 4);
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

/// An empty directory of that name under the temp directory, made fresh: what
/// a previous run left there is removed first.
///
/// The temp directory is TMP/TEMP/TMPDIR, which a build on a full system drive
/// points somewhere with room; nothing here names a drive.
inline std::filesystem::path scratchDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// `data` written to `path`, its directories made first.
inline void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

/// A WDBC file with `fields` uint32 columns; the strings block as given. A
/// float column is its bits (std::bit_cast<uint32_t>).
inline std::vector<uint8_t> wdbcFile(uint32_t fields,
                                     const std::vector<std::vector<uint32_t>>& rows,
                                     const std::string& strings) {
    std::vector<uint8_t> out = {'W', 'D', 'B', 'C'};
    auto u32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    u32(static_cast<uint32_t>(rows.size()));
    u32(fields);
    u32(fields * 4);
    u32(static_cast<uint32_t>(strings.size()));
    for (const auto& row : rows)
        for (uint32_t v : row) u32(v);
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

/// An extracted data root an AssetManager can open: the files on disk, keyed
/// by their archive path, under a manifest naming them.
inline std::filesystem::path makeDataRoot(
    const char* name, const std::map<std::string, std::vector<uint8_t>>& files) {
    const std::filesystem::path root = scratchDir(name);
    std::string entries;
    for (const auto& [key, data] : files) {
        std::string relative = key;
        for (char& c : relative) if (c == '\\') c = '/';
        writeFile(root / relative, data);
        std::string jsonKey;
        for (char c : key) {
            if (c == '\\') jsonKey += "\\\\";
            else jsonKey += c;
        }
        if (!entries.empty()) entries += ",";
        entries += "\"" + jsonKey + "\":{\"p\":\"" + relative + "\",\"s\":" +
                   std::to_string(data.size()) + ",\"h\":\"00000000\"}";
    }
    std::ofstream(root / "manifest.json")
        << "{\"version\":1,\"basePath\":\".\",\"entries\":{" << entries << "}}";
    return root;
}

/// The .dbc for a table, found case-insensitively, or an empty path.
///
/// The extracted archives do not agree about capitalisation - Spell.dbc,
/// spell.dbc and SPELL.DBC all occur - so the name is folded on both sides.
/// A copy that lost the fold finds the file on one machine and not another,
/// and the test that uses it then passes by skipping.
inline std::filesystem::path dbcPathFor(const std::string& table) {
    std::string wanted = table;
    for (char& c : wanted) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(sourceRoot() + "Data/db", ec)) {
        if (entry.path().extension() != ".dbc") continue;
        std::string stem = entry.path().stem().string();
        for (char& c : stem) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }
        if (stem == wanted) return entry.path();
    }
    return {};
}

}  // namespace wowee::test

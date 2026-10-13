// A realm's patch archives above the extracted data: which copy of a file the
// AssetManager reads, and a table read with the realm's over_ diff merged in.
//
// The AssetManager half runs against a layer made up here. The MPQ half
// builds real archives and needs StormLib, as the client does.
#include <catch_amalgamated.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "pipeline/archive_layer.hpp"
#include "pipeline/asset_manager.hpp"
#ifdef WOWEE_HAVE_STORMLIB
#include "pipeline/mpq_archive_layer.hpp"
#include <StormLib.h>
#endif

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wowee::pipeline;

namespace {

std::vector<uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }

fs::path makeScratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

/// WDBC with `fields` uint32 columns; the strings block as given.
std::vector<uint8_t> dbc(uint32_t fields, const std::vector<std::vector<uint32_t>>& rows,
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

/// An extracted data root: the files on disk under a manifest naming them.
fs::path makeDataRoot(const char* name, const std::map<std::string, std::vector<uint8_t>>& files) {
    const fs::path root = makeScratch(name);
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

class FakeLayer final : public ArchiveLayer {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool contains(const std::string& path) const override { return files.count(path) != 0; }
    std::vector<uint8_t> read(const std::string& path) const override {
        auto it = files.find(path);
        return it == files.end() ? std::vector<uint8_t>{} : it->second;
    }
};

}  // namespace

TEST_CASE("The archive layer answers before the extracted data", "[archive_layer]") {
    const fs::path root = makeDataRoot("wowee_archive_layer_order", {
        {"creature\\bear\\bear.m2", bytes("extracted bear")},
        {"creature\\wolf\\wolf.m2", bytes("extracted wolf")},
    });
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    REQUIRE(assets.readFile("Creature\\Bear\\Bear.m2") == bytes("extracted bear"));

    auto layer = std::make_shared<FakeLayer>();
    layer->files["creature\\bear\\bear.m2"] = bytes("realm bear");
    layer->files["creature\\realmonly\\realmonly.m2"] = bytes("realm only");
    assets.setArchiveLayer(layer);

    // Replaced, though the extracted copy was read - and cached - above.
    REQUIRE(assets.readFile("Creature\\Bear\\Bear.m2") == bytes("realm bear"));
    REQUIRE(assets.readFile("creature/realmonly/realmonly.m2") == bytes("realm only"));
    REQUIRE(assets.fileExists("creature\\realmonly\\realmonly.m2"));
    // Not the layer's: the extracted file stands.
    REQUIRE(assets.readFile("creature\\wolf\\wolf.m2") == bytes("extracted wolf"));
    // A path the layer holds has no file on disk to look beside.
    REQUIRE(assets.resolveFile("creature\\bear\\bear.m2").empty());
    REQUIRE_FALSE(assets.resolveFile("creature\\wolf\\wolf.m2").empty());

    // Taken away again, everything is as extracted.
    assets.setArchiveLayer(nullptr);
    REQUIRE(assets.readFile("creature\\bear\\bear.m2") == bytes("extracted bear"));
    REQUIRE_FALSE(assets.fileExists("creature\\realmonly\\realmonly.m2"));
}

TEST_CASE("The override directory still comes before the archive layer", "[archive_layer]") {
    const fs::path root = makeDataRoot("wowee_archive_layer_override", {
        {"textures\\sky.blp", bytes("extracted sky")},
    });
    writeFile(root / "override" / "textures" / "sky.blp", bytes("player's sky"));
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    auto layer = std::make_shared<FakeLayer>();
    layer->files["textures\\sky.blp"] = bytes("realm sky");
    assets.setArchiveLayer(layer);
    REQUIRE(assets.readFile("textures\\sky.blp") == bytes("player's sky"));
}

TEST_CASE("A table is read with the realm's over_ diff merged by ID", "[archive_layer][dbc]") {
    // ID, SpellVisual, name.
    const fs::path root = makeDataRoot("wowee_archive_layer_dbc", {
        {"dbfilesclient\\spell.dbc",
         dbc(3, {{15473, 100, 1}, {133, 200, 12}}, std::string("\0Shadowform\0Fireball\0", 21))},
    });
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    REQUIRE(assets.loadDBC("Spell.dbc")->getRecordCount() == 2);

    auto layer = std::make_shared<FakeLayer>();
    layer->files["dbfilesclient\\over_spell.dbc"] =
        dbc(3, {{107904, 10150, 1}, {133, 201, 0}}, std::string("\0Shadowform\0", 12));
    // A whole table shipped by the realm replaces the client's outright.
    layer->files["dbfilesclient\\charbaseinfo.dbc"] = dbc(1, {{7}}, std::string(1, '\0'));
    assets.setArchiveLayer(layer);

    auto spells = assets.loadDBC("Spell.dbc");
    REQUIRE(spells);
    REQUIRE(spells->getRecordCount() == 3);
    const int32_t base = spells->findRecordById(15473);
    REQUIRE(base >= 0);
    REQUIRE(spells->getUInt32(static_cast<uint32_t>(base), 1) == 100);
    const int32_t changed = spells->findRecordById(133);
    REQUIRE(changed >= 0);
    REQUIRE(spells->getUInt32(static_cast<uint32_t>(changed), 1) == 201);
    REQUIRE(spells->getString(static_cast<uint32_t>(changed), 2).empty());
    const int32_t added = spells->findRecordById(107904);
    REQUIRE(added >= 0);
    REQUIRE(spells->getUInt32(static_cast<uint32_t>(added), 1) == 10150);
    REQUIRE(spells->getString(static_cast<uint32_t>(added), 2) == "Shadowform");

    auto whole = assets.loadDBC("CharBaseInfo.dbc");
    REQUIRE(whole);
    REQUIRE(whole->getRecordCount() == 1);

    // And back to the client's own when the realm's archives are gone.
    assets.setArchiveLayer(nullptr);
    REQUIRE(assets.loadDBC("Spell.dbc")->getRecordCount() == 2);
}


#ifdef WOWEE_HAVE_STORMLIB
namespace {

/// A real MPQ holding `files`, each written to disk first and added the way
/// mpq_build adds them.
bool packArchive(const fs::path& archivePath, const std::map<std::string, std::string>& files) {
    const fs::path staging = archivePath.parent_path() / (archivePath.stem().string() + "_files");
    HANDLE archive = nullptr;
    if (!SFileCreateArchive(archivePath.string().c_str(), MPQ_CREATE_ARCHIVE_V1 | MPQ_CREATE_LISTFILE,
                            64, &archive)) {
        return false;
    }
    bool ok = true;
    int n = 0;
    for (const auto& [name, content] : files) {
        const fs::path source = staging / std::to_string(n++);
        writeFile(source, bytes(content));
        ok = ok && SFileAddFileEx(archive, source.string().c_str(), name.c_str(),
                                  MPQ_FILE_REPLACEEXISTING | MPQ_FILE_COMPRESS,
                                  MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_NEXT_SAME);
    }
    SFileCloseArchive(archive);
    return ok;
}

}  // namespace

TEST_CASE("A later archive's copy of a file replaces an earlier one's", "[archive_layer][mpq]") {
#ifdef __linux__
    // See test_extract_progress: Ubuntu's StormLib aborts writing an archive.
    SKIP("Ubuntu's StormLib build asserts when creating an archive");
#endif
    const fs::path root = makeScratch("wowee_archive_layer_mpq");
    const fs::path first = root / "new_creature_models.MPQ";
    const fs::path second = root / "hotfixes.MPQ";
    REQUIRE(packArchive(first, {{"Creature\\Bear\\Bear.m2", "first bear"},
                                {"Creature\\Wolf\\Wolf.m2", "first wolf"}}));
    REQUIRE(packArchive(second, {{"Creature\\Bear\\Bear.m2", "hotfixed bear"}}));

    std::string error;
    auto layer = MpqArchiveLayer::open({first, second}, error);
    REQUIRE(layer);
    REQUIRE(error.empty());
    REQUIRE(layer->fileCount() == 2);
    // Paths come out normalized, the archive's own metadata left out.
    REQUIRE(layer->contains("creature\\bear\\bear.m2"));
    REQUIRE_FALSE(layer->contains("(listfile)"));
    REQUIRE(layer->read("creature\\bear\\bear.m2") == bytes("hotfixed bear"));
    REQUIRE(layer->read("creature\\wolf\\wolf.m2") == bytes("first wolf"));
    REQUIRE(layer->read("creature\\cat\\cat.m2").empty());
    REQUIRE(layer->filesPerArchive() == std::vector<size_t>{1, 1});

    // In the other order the first archive wins instead.
    auto reversed = MpqArchiveLayer::open({second, first}, error);
    REQUIRE(reversed);
    REQUIRE(reversed->read("creature\\bear\\bear.m2") == bytes("first bear"));
}

TEST_CASE("A stack with an archive missing is not opened at all", "[archive_layer][mpq]") {
#ifdef __linux__
    SKIP("Ubuntu's StormLib build asserts when creating an archive");
#endif
    const fs::path root = makeScratch("wowee_archive_layer_missing");
    const fs::path present = root / "present.MPQ";
    REQUIRE(packArchive(present, {{"a.txt", "a"}}));
    std::string error;
    REQUIRE_FALSE(MpqArchiveLayer::open({present, root / "absent.MPQ"}, error));
    REQUIRE(error.find("absent.MPQ") != std::string::npos);
}
#endif

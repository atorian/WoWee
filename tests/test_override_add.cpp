// The override directory, for files the expansion never had.
//
// A pack may add as well as replace - tools/asset_pack_curate.py states the
// rule and tools/asset_pack_from_client.py builds packs that rely on it. A
// replacement resolves through the manifest entry it supersedes, but an
// addition has no entry to resolve against: Cataclysm's leaf atlas arrives
// under a name 3.3.5 never had. Resolution used to stop there, so a model
// dropped in as an override rendered white, its own new textures unreachable
// while it was not.
//
// Each case builds a small data tree - a manifest, the extracted files it
// names, and an override directory - and reads through a real AssetManager,
// as the renderer does.
#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "pipeline/asset_manager.hpp"
#include "test_support.hpp"

using wowee::pipeline::AssetManager;

namespace {

std::vector<uint8_t> bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string text(const std::vector<uint8_t>& data) {
    return std::string(data.begin(), data.end());
}

}  // namespace

TEST_CASE("an override replaces a file the manifest knows") {
    // The manifest files the asset under a path of its own, as an extraction
    // does, and the pack replaces it under that same path.
    const auto root = wowee::test::scratchDir("wowee_override_replace");
    wowee::test::writeFile(root / "manifest.json",
                           bytes(R"({"version":1,"basePath":".","entries":{)"
                                 R"("world\\trees\\old.blp":{"p":"extracted/old_tree.blp","s":4,"h":"00000000"}}})"));
    wowee::test::writeFile(root / "extracted/old_tree.blp", bytes("base"));
    wowee::test::writeFile(root / "override/extracted/old_tree.blp", bytes("pack"));
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));

    CHECK(text(assets.readFile("world\\trees\\old.blp")) == "pack");
}

TEST_CASE("an override adds a file the manifest has never heard of") {
    // No manifest entry, no file in the tree - only the pack has it.
    const auto root = wowee::test::makeDataRoot("wowee_override_add",
                                                {{"world\\trees\\old.blp", bytes("base")}});
    wowee::test::writeFile(root / "override/world/trees/leaves_set.blp", bytes("pack"));
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));

    CHECK(text(assets.readFile("world\\trees\\leaves_set.blp")) == "pack");
}

TEST_CASE("without the override, the manifest still answers") {
    const auto root = wowee::test::makeDataRoot("wowee_override_none",
                                                {{"world\\trees\\old.blp", bytes("base")}});
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));

    CHECK(text(assets.readFile("world\\trees\\old.blp")) == "base");
}

TEST_CASE("a path nothing has resolves to nothing") {
    const auto root = wowee::test::makeDataRoot("wowee_override_absent",
                                                {{"world\\trees\\old.blp", bytes("base")}});
    wowee::test::writeFile(root / "override/world/trees/other.blp", bytes("pack"));
    AssetManager assets;
    REQUIRE(assets.initialize(root.string()));

    CHECK_FALSE(assets.fileExists("world\\trees\\absent.blp"));
    CHECK(assets.readFileOptional("world\\trees\\absent.blp").empty());
}

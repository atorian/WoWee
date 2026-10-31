#include <catch_amalgamated.hpp>

#include "game/expansion_profile.hpp"

#include <filesystem>
#include <fstream>

namespace {

/// A profile tree whose expansion.json carries `extra` alongside the basics.
std::filesystem::path writeProfileWith(const std::string& extra) {
    const auto root = std::filesystem::temp_directory_path() /
                      "wowee_expansion_warden_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "expansions" / "classic");
    std::ofstream out(root / "expansions" / "classic" / "expansion.json");
    out << R"({
        "id": "classic",
        "name": "Classic",
        "shortName": "Classic",
        "version": { "major": 1, "minor": 12, "patch": 1 },
        "build": 5875,
        "protocolVersion": 8)" << extra << "\n    }";
    return root;
}

} // namespace

TEST_CASE("Expansion registry prefers a profile with extracted assets",
          "[expansion_profile]") {
    const auto root = std::filesystem::temp_directory_path() /
                      "wowee_expansion_asset_selection_test";
    std::filesystem::remove_all(root);
    for (const auto& [id, build] : {
             std::pair<const char*, int>{"classic", 5875},
             {"tbc", 8606},
             {"wotlk", 12340}}) {
        const auto dir = root / "expansions" / id;
        std::filesystem::create_directories(dir);
        std::ofstream profile(dir / "expansion.json");
        profile << "{\"id\":\"" << id << "\",\"name\":\"" << id
                << "\",\"shortName\":\"" << id << "\",\"build\":"
                << build << ",\"protocolVersion\":3}";
    }
    std::ofstream(root / "expansions" / "tbc" / "manifest.json") << "{}";

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 3);
    REQUIRE(registry.getActive() != nullptr);
    CHECK(registry.getActive()->id == "tbc");

    std::filesystem::remove_all(root);
}

TEST_CASE("A realm's own Warden key is read from its profile", "[expansion_profile]") {
    // 512 hex characters, which is the only length a 2048-bit modulus has.
    const std::string key(512, 'a');
    const auto root = writeProfileWith(",\n        \"wardenRsaModulus\": \"" + key + "\"");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("classic");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->wardenRsaModulus.size() == 256);
    CHECK(profile->wardenRsaModulus.front() == 0xAA);
    CHECK(profile->wardenRsaModulus.back() == 0xAA);

    std::filesystem::remove_all(root);
}

TEST_CASE("A Warden key of the wrong length is refused rather than half-read",
          "[expansion_profile]") {
    // One nibble short. Taking 255 bytes and a half would fail verification
    // exactly the way no key at all does, which is the one outcome that would
    // send somebody looking anywhere but at the profile.
    const auto root = writeProfileWith(",\n        \"wardenRsaModulus\": \"" +
                                       std::string(511, 'a') + "\"");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("classic");
    REQUIRE(profile != nullptr);
    CHECK(profile->wardenRsaModulus.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("A Warden key with a character that is not hex is refused",
          "[expansion_profile]") {
    const auto root = writeProfileWith(",\n        \"wardenRsaModulus\": \"" +
                                       std::string(511, 'a') + "z\"");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("classic");
    REQUIRE(profile != nullptr);
    CHECK(profile->wardenRsaModulus.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("A profile naming no Warden key leaves the retail one in place",
          "[expansion_profile]") {
    const auto root = writeProfileWith("");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("classic");
    REQUIRE(profile != nullptr);
    CHECK(profile->wardenRsaModulus.empty());

    std::filesystem::remove_all(root);
}

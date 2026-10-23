#include "ezwow_patch_fixture.hpp"
#include "ezwow_loader_fixture.hpp"
#include <catch_amalgamated.hpp>
#include "game/server_extension.hpp"
#include "core/env.hpp"
#include "ezwow_bootstrap_fixture.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <bit>
#include <limits>
#include <future>
namespace {
namespace fs = std::filesystem;
using wowee::game::ServerExtension;
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("wowee_extensions_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::path adapter = fs::absolute(EXTENSION_FIXTURE_PATH);
    fs::path executable = fs::absolute(EXTENSION_HOST_PATH);
    Fixture() {
        if (const auto* path = std::getenv("WOWEE_TEST_EXTENSION_HOST")) executable = fs::absolute(path);
        if (const auto* path = std::getenv("WOWEE_TEST_EXTENSION_ADAPTER")) adapter = fs::absolute(path);
        fs::create_directories(root);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    nlohmann::json profile() const {
        std::ifstream stream(adapter, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
        unsigned char hash[EVP_MAX_MD_SIZE]; unsigned size = 0;
        EVP_Digest(bytes.data(), bytes.size(), hash, &size, EVP_sha256(), nullptr);
        std::string hex; constexpr char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < size; ++i) { hex += digits[hash[i] >> 4]; hex += digits[hash[i] & 15]; }
        const auto utf8 = adapter.u8string();
        const std::string adapterPath(utf8.begin(), utf8.end());
        return {{"version", 1}, {"provider", "fixture"}, {"build", 12340},
                {"adapter", {{"path", adapterPath}, {"sha256", hex}}},
                {"library", {{"path", adapterPath}, {"sha256", hex}}}};
    }
    void install(const std::string& host, const nlohmann::json& profile) const {
        const auto directory = root / wowee::game::serverExtensionKey(host, 3724);
        fs::create_directories(directory);
        std::ofstream(directory / "profile.json") << profile.dump();
    }
};
}
TEST_CASE("Server extensions use canonical endpoint identities", "[extension]") {
    using wowee::game::serverExtensionKey;
    CHECK(serverExtensionKey(" Login.Example.org \t", 3724) == serverExtensionKey("login.example.org", 3724));
    CHECK(serverExtensionKey("login.example.org", 3724) != serverExtensionKey("login.example.org", 3725));
    CHECK(serverExtensionKey("a/b", 3724) != serverExtensionKey("a?b", 3724));
    CHECK(serverExtensionKey("../../CON", 3724).find("..") == std::string::npos);
    CHECK(serverExtensionKey("../../CON", 3724).find('/') == std::string::npos);
}
TEST_CASE("Switching servers starts a fresh adapter and clears runtime memory", "[extension]") {
    Fixture f; f.install("alpha.example", f.profile()); f.install("beta.example", f.profile());
    ServerExtension runtime;
    REQUIRE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    REQUIRE(runtime.active()); uint8_t result[4]{};
    REQUIRE(runtime.readMemory("fixture.dll", 0x1000, 4, result) == ServerExtension::ReadResult::Handled);
    CHECK(result[0] == 1); CHECK(result[1] == 'a');
    REQUIRE(runtime.readMemory("fixture.dll", 0x1000, 4, result) == ServerExtension::ReadResult::Handled);
    CHECK(result[0] == 2);
    REQUIRE(runtime.begin(f.root, "beta.example", 3724, 12340, f.executable));
    REQUIRE(runtime.readMemory("fixture.dll", 0x1000, 4, result) == ServerExtension::ReadResult::Handled);
    CHECK(result[0] == 1); CHECK(result[1] == 'b');
    REQUIRE(runtime.begin(f.root, "stock.example", 3724, 12340, f.executable));
    CHECK_FALSE(runtime.active()); CHECK(runtime.error().empty()); CHECK(runtime.clientPath().empty());
    CHECK(runtime.readMemory("fixture.dll", 0x1000, 4, result) == ServerExtension::ReadResult::Unhandled);
}
TEST_CASE("A realm's profile is found in the data folder the client reads, not only in Data/", "[extension]") {
    // A client started with WOW_DATA_PATH and no Data/ in its working
    // directory read its assets and never the realm's profile, answered the
    // realm's own Warden checks as stock WoW, and waited for a character list
    // that never came.
    Fixture f; f.install("data-root.example", f.profile());
    const auto dataRoot = f.root.parent_path() / (f.root.filename().string() + "_data");
    fs::create_directories(dataRoot);
    fs::rename(f.root, dataRoot / "server_extensions");
    const auto* oldOverride = std::getenv("WOWEE_SERVER_EXTENSIONS_DIR");
    const auto* oldData = std::getenv("WOW_DATA_PATH");
    const std::string savedOverride = oldOverride ? oldOverride : "", savedData = oldData ? oldData : "";
    wowee::core::unsetEnvVar("WOWEE_SERVER_EXTENSIONS_DIR");
    wowee::core::setEnvVar("WOW_DATA_PATH", dataRoot.string().c_str());

    const auto found = wowee::game::locateServerExtension("data-root.example", 3724);
    const auto stock = wowee::game::locateServerExtension("stock.example", 3724);
    ServerExtension runtime; uint8_t result[4]{};
    const bool begun = !found.root.empty() &&
        runtime.begin(found.root, "data-root.example", 3724, 12340, f.executable);
    const auto read = begun ? runtime.readMemory("fixture.dll", 0x1000, 4, result)
                            : ServerExtension::ReadResult::Unhandled;

    savedOverride.empty() ? wowee::core::unsetEnvVar("WOWEE_SERVER_EXTENSIONS_DIR")
                          : wowee::core::setEnvVar("WOWEE_SERVER_EXTENSIONS_DIR", savedOverride.c_str());
    savedData.empty() ? wowee::core::unsetEnvVar("WOW_DATA_PATH")
                      : wowee::core::setEnvVar("WOW_DATA_PATH", savedData.c_str());
    runtime.reset();
    std::error_code ec; fs::remove_all(dataRoot, ec);

    REQUIRE(begun);
    CHECK(read == ServerExtension::ReadResult::Handled);
    CHECK(stock.root.empty());
    CHECK_FALSE(stock.searched.empty());
}
TEST_CASE("A server library cannot be used with the wrong build or checksum", "[extension]") {
    Fixture f; ServerExtension runtime;
    SECTION("wrong build") {
        f.install("alpha.example", f.profile());
        CHECK_FALSE(runtime.begin(f.root, "alpha.example", 3724, 5875, f.executable));
    }
    SECTION("bad checksum") {
        auto profile = f.profile(); profile["adapter"]["sha256"] = std::string(64, '0');
        f.install("alpha.example", profile);
        CHECK_FALSE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    }
    SECTION("native DLL without an adapter") {
        auto profile = f.profile(); profile.erase("adapter"); profile["provider"] = "EZWoW";
        f.install("alpha.example", profile);
        CHECK_FALSE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
        CHECK(runtime.error().find("EZWoW") != std::string::npos);
    }
    CHECK_FALSE(runtime.active()); CHECK_FALSE(runtime.error().empty());
}
TEST_CASE("Adapter termination and timeout fail without returning static bytes", "[extension]") {
    Fixture f; f.install("alpha.example", f.profile()); ServerExtension runtime;
    REQUIRE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    const auto started = std::chrono::steady_clock::now(); uint8_t result = 0xCC;
    const auto address = GENERATE(0xDEAD, 0xD1ED);
    CHECK(runtime.readMemory("fixture.dll", address, 1, &result) == ServerExtension::ReadResult::Error);
    CHECK(result == 0xCC); CHECK_FALSE(runtime.active()); CHECK_FALSE(runtime.error().empty());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(6));
}

TEST_CASE("Required runtime memory never falls back to zero-filled PE data", "[extension]") {
    Fixture f; auto profile = f.profile();
    profile["runtime_ranges"] = {{{"address", 0x2000}, {"size", 16}}};
    f.install("alpha.example", profile); ServerExtension runtime;
    REQUIRE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    uint8_t result[4]{0xCC, 0xCC, 0xCC, 0xCC};
    CHECK(runtime.readMemory("", 0x1000, 4, result) == ServerExtension::ReadResult::Unhandled);
    CHECK(runtime.readMemory("", 0x1FFF, 4, result) == ServerExtension::ReadResult::Error);
    CHECK(result[0] == 0xCC); CHECK_FALSE(runtime.active());
}

TEST_CASE("Adapters load from Unicode paths containing spaces", "[extension]") {
    Fixture f;
    const auto directory = f.root / fs::path(u8"сервер с пробелом");
    fs::create_directories(directory);
    const auto original = f.adapter;
    f.adapter = directory / fs::path(u8"адаптер.dll");
    fs::copy_file(original, f.adapter);
    f.install("alpha.example", f.profile()); ServerExtension runtime;
    REQUIRE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    CHECK(runtime.active());
}

TEST_CASE("Built-in server protocols need no native DLL and reset across endpoints", "[protocol]") {
    Fixture f;
    auto profile = nlohmann::json{{"version",1},{"provider","EZWoW"},{"build",12340},
        {"protocol","ezwow"},{"trace",true}};
    f.install("alpha.example", profile);
    ServerExtension runtime;
    REQUIRE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.root / "missing-host"));
    CHECK(runtime.active());
    uint8_t memory = 0xCC;
    CHECK(runtime.readMemory("", 0x4002A7, 1, &memory) == ServerExtension::ReadResult::Unhandled);
    CHECK(memory == 0xCC);
    bool sent = false;
    const auto send = [&](uint16_t, const std::vector<uint8_t>&) { sent = true; };
    CHECK(runtime.packet(0x1EE, {}, send) == wowee::game::ProtocolResult::Unhandled);
    runtime.observeWarden({});
    CHECK(runtime.packet(0x41D, {0,0,0,0,0,0,0,0,1,0,0,0,0xC3}, send) == wowee::game::ProtocolResult::Error);
    CHECK_FALSE(sent);
    CHECK(runtime.error().find("bootstrap") != std::string::npos);
    REQUIRE(runtime.begin(f.root, "stock.example", 3724, 12340, f.executable));
    CHECK_FALSE(runtime.active()); CHECK(runtime.error().empty());
    CHECK(runtime.packet(0x41D, {}, send) == wowee::game::ProtocolResult::Unhandled);
    REQUIRE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    CHECK(runtime.readMemory("WoW.exe", 0xDD0FFF, 2, &memory) == ServerExtension::ReadResult::Error);
    CHECK(memory == 0xCC);
    CHECK(runtime.error().find("bootstrap") != std::string::npos);
    profile["protocol"] = "unknown"; f.install("alpha.example", profile);
    CHECK_FALSE(runtime.begin(f.root, "alpha.example", 3724, 12340, f.executable));
    CHECK_FALSE(runtime.active());
}
TEST_CASE("EZWoW packet offsets cannot wrap or read outside the payload", "[protocol]") {
    using wowee::game::parseEzWowPayload;
    const std::vector<uint8_t> body{0,0,0,0,2,0,0,0,0x4C,0x45,2,0,0,0,0x90,0xC3,0xFF};
    const auto result = parseEzWowPayload(body);
    CHECK(result.command == 0);
    REQUIRE(result.arguments.size() == 2); CHECK(result.arguments[0] == 0x4C);
    REQUIRE(result.code.size() == 2); CHECK(result.code[1] == 0xC3);
    REQUIRE(result.trailing.size() == 1); CHECK(result.trailing[0] == 0xFF);
    for (size_t n = 0; n < body.size() - 1; ++n)
        CHECK_THROWS(parseEzWowPayload(std::span<const uint8_t>(body).first(n)));
    auto invalid = body;
    std::fill_n(invalid.begin() + 4, 4, 0xFF); CHECK_THROWS(parseEzWowPayload(invalid));
    invalid = body; std::fill_n(invalid.begin() + 10, 4, 0xFF); CHECK_THROWS(parseEzWowPayload(invalid));
    invalid = body; std::fill_n(invalid.begin() + 10, 4, 0); CHECK_THROWS(parseEzWowPayload(invalid));
    CHECK_THROWS(parseEzWowPayload(std::vector<uint8_t>(1024*1024+1)));
}
TEST_CASE("Protocol traces exclude auth and Warden keys and stop at the cap", "[protocol]") {
    Fixture f; fs::path path;
    {
        wowee::game::ServerProtocolTrace trace(f.root / "trace"); path = trace.path();
        trace.record(2, 0x1ED, { });
        const std::vector<uint8_t> moduleKey{0,1,2,3};
        trace.record(1, 0x2E6, moduleKey);
        trace.record(1, 0x2E6, std::vector<uint8_t>{5,9,9,9});
        CHECK(fs::file_size(path) == 4);
        trace.record(1, 0x2E6, std::vector<uint8_t>{3,4});
        CHECK(fs::file_size(path) == 18);
        trace.record(2, 0x23D, std::vector<uint8_t>{0,1});
        CHECK(fs::file_size(path) == 32);
        const std::vector<uint8_t> large(1024*1024, 2);
        for (int i = 0; i < 12; ++i) trace.record(1, 0x2E6, large);
        const auto size = fs::file_size(path);
        CHECK(size <= 8*1024*1024); CHECK(size > 7*1024*1024);
        trace.record(1, 0x2E6, large); CHECK(fs::file_size(path) == size);
    }
    std::ifstream stream(path, std::ios::binary);
    char magic[4]{}; stream.read(magic, 4); CHECK(std::string(magic,4) == "WPT1");
    CHECK(stream.get() == 1); // first record is Warden initialization
}

namespace {
std::vector<uint8_t> initializeTiming(uint32_t function) {
    std::vector<uint8_t> body{1,1,0};
    for(int i=0;i<4;++i) body.push_back(uint8_t(function>>(8*i)));
    body.push_back(1);
    unsigned char hash[EVP_MAX_MD_SIZE]{}; unsigned size=0;
    EVP_Digest(body.data(), body.size(), hash, &size, EVP_sha1(), nullptr);
    uint32_t checksum=0;
    for(int i=0;i<5;++i) for(int j=0;j<4;++j) checksum ^= uint32_t(hash[i*4+j]) << (8*j);
    std::vector<uint8_t> frame{3,8,0};
    for(int i=0;i<4;++i) frame.push_back(uint8_t(checksum>>(8*i)));
    frame.insert(frame.end(), body.begin(), body.end());return frame;
}
std::vector<uint8_t> bootstrapWrite(size_t index, bool corrupt=false) {
    const uint32_t count = uint32_t((0xDD1000+index*8-0xBEA170)/8+1);
    std::vector<uint8_t> body{0,0,0,0};
    for(int i=0;i<4;++i) body.push_back(uint8_t(count>>(8*i)));
    body.insert(body.end(), ezBootstrapFixture+index*8, ezBootstrapFixture+index*8+8);
    if(corrupt) body[8]^=1;
    return body;
}
}
TEST_CASE("EZWoW installs only the bootstrap delivered by this session", "[protocol]") {
    auto protocol = wowee::game::makeServerProtocol("ezwow", {"login.ezwow.org",3724,12340});
    const auto send = [](uint16_t,const std::vector<uint8_t>&) { FAIL("Bootstrap writes send no world reply"); };
    CHECK(protocol->packet(0x2E9,bootstrapWrite(0),send)==wowee::game::ProtocolResult::Unhandled);
    protocol->warden(initializeTiming(0x14E720));
    for(size_t i=27;i-- >0;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==wowee::game::ProtocolResult::Handled);
    protocol->warden(initializeTiming(0x46AE20)); REQUIRE(protocol->error().empty());
    uint8_t result[216]{};
    REQUIRE(protocol->memory("",0xDD1000,216,result)==wowee::game::ProtocolResult::Handled);
    CHECK(std::equal(std::begin(result),std::end(result),std::begin(ezBootstrapFixture)));
    REQUIRE(protocol->memory("WoW.exe",0xDD104D,10,result)==wowee::game::ProtocolResult::Handled);
    CHECK(std::equal(result,result+10,ezBootstrapFixture+0x4D));
    protocol->warden(initializeTiming(0x14E720));
    CHECK(protocol->memory("",0xDD1000,1,result)==wowee::game::ProtocolResult::Error);
}
TEST_CASE("EZWoW rejects changed code and incomplete or corrupt initialization", "[protocol]") {
    const auto send = [](uint16_t,const std::vector<uint8_t>&) {};
    auto protocol = wowee::game::makeServerProtocol("ezwow", {"login.ezwow.org",3724,12340});
    SECTION("unverified bootstrap variant") {
        protocol->warden(initializeTiming(0x14E720));
        for(size_t i=27;i-- >1;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==wowee::game::ProtocolResult::Handled);
        CHECK(protocol->packet(0x2E9,bootstrapWrite(0,true),send)==wowee::game::ProtocolResult::Error);
    }
    SECTION("partial bootstrap") {
        protocol->warden(initializeTiming(0x14E720));
        REQUIRE(protocol->packet(0x2E9,bootstrapWrite(0),send)==wowee::game::ProtocolResult::Handled);
        protocol->warden(initializeTiming(0x46AE20)); uint8_t result=0xCC;
        CHECK(protocol->memory("",0xDD1000,1,&result)==wowee::game::ProtocolResult::Error); CHECK(result==0xCC);
    }
    SECTION("bad initialization checksum") {
        auto init=initializeTiming(0x14E720); init[3]^=1; protocol->warden(init);
        CHECK(protocol->packet(0x2E9,bootstrapWrite(0),send)==wowee::game::ProtocolResult::Unhandled);
        CHECK_FALSE(protocol->error().empty());
    }
    SECTION("truncated initialization") {
        protocol->warden(std::vector<uint8_t>{3,8,0,1}); CHECK_FALSE(protocol->error().empty());
    }
}

TEST_CASE("EZWoW translates the verified loader into one patch request per session", "[protocol]") {
    using wowee::game::ProtocolResult;
    auto protocol = wowee::game::makeServerProtocol("ezwow", {"login.ezwow.org",3724,12340});
    std::vector<std::pair<uint16_t,std::vector<uint8_t>>> replies;
    const auto send = [&](uint16_t opcode,const std::vector<uint8_t>& body) { replies.emplace_back(opcode,body); };
    const std::vector<uint8_t> loader(std::begin(ezLoaderFixture),std::end(ezLoaderFixture));
    const auto parsed=wowee::game::parseEzWowPayload(loader);
    CHECK(parsed.arguments.size()==37); CHECK(parsed.code.size()==8088); CHECK(parsed.trailing.empty());
    protocol->warden(initializeTiming(0x14E720));
    for(size_t i=27;i-- >0;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==ProtocolResult::Handled);
    protocol->warden(initializeTiming(0x46AE20));
    SECTION("valid loader and retransmission") {
        REQUIRE(protocol->packet(0x41D,loader,send)==ProtocolResult::Handled);
        REQUIRE(replies.size()==1); CHECK(replies[0].first==0x51F); CHECK(replies[0].second.empty());
        CHECK(protocol->error().empty());
        const std::vector<uint8_t> manifest(std::begin(ezPatchFixture),std::end(ezPatchFixture));
        CHECK(protocol->packet(0x41D,manifest,send)==ProtocolResult::Handled); CHECK(replies.size()==1);
        REQUIRE(protocol->packet(0x41D,loader,send)==ProtocolResult::Handled); CHECK(replies.size()==1);
        protocol->warden(initializeTiming(0x14E720));
        for(size_t i=27;i-- >0;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==ProtocolResult::Handled);
        protocol->warden(initializeTiming(0x46AE20));
        CHECK(protocol->packet(0x41D,loader,send)==ProtocolResult::Handled); CHECK(replies.size()==2);
    }
    SECTION("changed executable code") {
        auto changed=loader; changed.back()^=1;
        CHECK(protocol->packet(0x41D,changed,send)==ProtocolResult::Error); CHECK(replies.empty());
    }
    SECTION("different library release") {
        auto changed=loader; changed[8+21]^=1;
        CHECK(protocol->packet(0x41D,changed,send)==ProtocolResult::Error); CHECK(replies.empty());
    }
    SECTION("later commands use their own framing") {
        CHECK(protocol->packet(0x41D,std::vector<uint8_t>{2,0,0,0,9,8,7},send)==ProtocolResult::Error);
        CHECK(protocol->error().find("extension command 2 (3 bytes)")!=std::string::npos); CHECK(replies.empty());
    }
}

TEST_CASE("EZWoW enumeration retry follows only the final verified initialization", "[protocol]") {
    using wowee::game::ProtocolResult;
    auto protocol = wowee::game::makeServerProtocol("ezwow", {"login.ezwow.org",3724,12340});
    const auto send = [](uint16_t opcode,const std::vector<uint8_t>& body) {
        REQUIRE(opcode==0x51F); REQUIRE(body.empty());
    };
    const std::vector<uint8_t> finalInit{0x03,0x14,0x00,0x64,0xe5,0x02,0xea,0x01,0x00,0x02,0x00,0x10,0x92,0x41,0x00,0xc0,0x18,0x02,0x00,0x30,0x25,0x02,0x00,0x10,0x29,0x02,0x00,0x03,0x08,0x00,0x76,0x9a,0xdd,0xfa,0x04,0x00,0x00,0x10,0x92,0x41,0x00,0x01,0x03,0x08,0x00,0xb4,0xe9,0xd7,0xba,0x01,0x01,0x00,0x20,0xae,0x46,0x00,0x01};
    const std::vector<uint8_t> loader(std::begin(ezLoaderFixture),std::end(ezLoaderFixture));
    const std::vector<uint8_t> manifest(std::begin(ezPatchFixture),std::end(ezPatchFixture));
    CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    protocol->warden(finalInit); CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    protocol->warden(initializeTiming(0x14E720));
    for(size_t i=27;i-- >0;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==ProtocolResult::Handled);
    protocol->warden(initializeTiming(0x46AE20));
    CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    REQUIRE(protocol->packet(0x41D,loader,send)==ProtocolResult::Handled);
    protocol->warden(finalInit); CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    REQUIRE(protocol->packet(0x41D,manifest,send)==ProtocolResult::Handled);
    protocol->warden(initializeTiming(0x46AE20));
    CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    SECTION("verified boundary is consumed once, including retransmission") {
        protocol->warden(finalInit); REQUIRE(protocol->error().empty());
        CHECK(protocol->takeCharacterListRetryRequest());
        CHECK_FALSE(protocol->takeCharacterListRetryRequest());
        protocol->warden(finalInit); CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    }
    SECTION("bad checksum cannot queue a retry") {
        auto changed=finalInit; changed.back()^=1; protocol->warden(changed);
        CHECK_FALSE(protocol->error().empty()); CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    }
    SECTION("truncation after valid records cannot queue a retry") {
        auto changed=finalInit; changed.pop_back(); protocol->warden(changed);
        CHECK_FALSE(protocol->error().empty()); CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    }
    SECTION("a later initialization error suppresses a pending retry") {
        protocol->warden(finalInit);
        auto changed=finalInit; changed.back()^=1; protocol->warden(changed);
        CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    }
    SECTION("restarting the bootstrap discards a pending retry") {
        protocol->warden(finalInit); protocol->warden(initializeTiming(0x14E720));
        CHECK_FALSE(protocol->takeCharacterListRetryRequest());
        protocol->warden(finalInit); CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    }
}

TEST_CASE("EZWoW character query can overlap checks without consuming the final retry", "[protocol]") {
    using wowee::game::ProtocolResult;
    auto protocol = wowee::game::makeServerProtocol("ezwow", {"login.ezwow.org",3724,12340});
    const auto send = [](uint16_t, const std::vector<uint8_t>&) {};
    const std::vector<uint8_t> loader(std::begin(ezLoaderFixture),std::end(ezLoaderFixture));
    const std::vector<uint8_t> manifest(std::begin(ezPatchFixture),std::end(ezPatchFixture));
    CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
    protocol->warden(initializeTiming(0x14E720));
    for (size_t i=27;i-- >0;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==ProtocolResult::Handled);
    protocol->warden(initializeTiming(0x46AE20));
    CHECK(protocol->takeEarlyCharacterListRetryRequest());
    CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
    REQUIRE(protocol->packet(0x41D,loader,send)==ProtocolResult::Handled);
    CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
    SECTION("valid manifest queues one query, duplicates do not") {
        REQUIRE(protocol->packet(0x41D,manifest,send)==ProtocolResult::Handled);
        CHECK(protocol->takeEarlyCharacterListRetryRequest());
        CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
        CHECK_FALSE(protocol->takeCharacterListRetryRequest());
        REQUIRE(protocol->packet(0x41D,manifest,send)==ProtocolResult::Handled);
        CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
        const std::vector<uint8_t> finalInit{0x03,0x14,0x00,0x64,0xe5,0x02,0xea,0x01,0x00,0x02,0x00,0x10,0x92,0x41,0x00,0xc0,0x18,0x02,0x00,0x30,0x25,0x02,0x00,0x10,0x29,0x02,0x00,0x03,0x08,0x00,0x76,0x9a,0xdd,0xfa,0x04,0x00,0x00,0x10,0x92,0x41,0x00,0x01,0x03,0x08,0x00,0xb4,0xe9,0xd7,0xba,0x01,0x01,0x00,0x20,0xae,0x46,0x00,0x01};
        protocol->warden(finalInit);
        CHECK(protocol->takeCharacterListRetryRequest());
        CHECK_FALSE(protocol->takeCharacterListRetryRequest());
    }
    SECTION("invalid manifest cannot queue a query") {
        auto invalid = manifest; invalid.pop_back();
        CHECK(protocol->packet(0x41D,invalid,send)==ProtocolResult::Error);
        CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
    }
    SECTION("bootstrap restart discards a pending query") {
        REQUIRE(protocol->packet(0x41D,manifest,send)==ProtocolResult::Handled);
        protocol->warden(initializeTiming(0x14E720));
        CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
    }
    SECTION("a later protocol error suppresses a pending query") {
        REQUIRE(protocol->packet(0x41D,manifest,send)==ProtocolResult::Handled);
        auto invalid = manifest; invalid.pop_back();
        CHECK(protocol->packet(0x41D,invalid,send)==ProtocolResult::Error);
        CHECK_FALSE(protocol->takeEarlyCharacterListRetryRequest());
    }
}

TEST_CASE("Server extension defers Warden enumeration hints and clears them on endpoint change", "[extension]") {
    using wowee::game::ProtocolResult;
    Fixture f;
    f.install("login.ezwow.org", {{"version",1},{"provider","EZWoW"},{"build",12340},{"protocol","ezwow"}});
    ServerExtension runtime;
    REQUIRE(runtime.begin(f.root,"login.ezwow.org",3724,12340,f.executable));
    const auto send = [](uint16_t,const std::vector<uint8_t>&) {};
    runtime.observeWarden(initializeTiming(0x14E720));
    for(size_t i=27;i-- >0;) REQUIRE(runtime.packet(0x2E9,bootstrapWrite(i),send)==ProtocolResult::Handled);
    runtime.observeWarden(initializeTiming(0x46AE20));
    REQUIRE(runtime.packet(0x41D,{std::begin(ezLoaderFixture),std::end(ezLoaderFixture)},send)==ProtocolResult::Handled);
    REQUIRE(runtime.packet(0x41D,{std::begin(ezPatchFixture),std::end(ezPatchFixture)},send)==ProtocolResult::Handled);
    const std::vector<uint8_t> finalInit{0x03,0x14,0x00,0x64,0xe5,0x02,0xea,0x01,0x00,0x02,0x00,0x10,0x92,0x41,0x00,0xc0,0x18,0x02,0x00,0x30,0x25,0x02,0x00,0x10,0x29,0x02,0x00,0x03,0x08,0x00,0x76,0x9a,0xdd,0xfa,0x04,0x00,0x00,0x10,0x92,0x41,0x00,0x01,0x03,0x08,0x00,0xb4,0xe9,0xd7,0xba,0x01,0x01,0x00,0x20,0xae,0x46,0x00,0x01};
    auto worker=std::async(std::launch::async,[&] { runtime.observeWarden(finalInit); });
    worker.get();
    SECTION("poll on the main thread consumes the worker hint once") {
        CHECK(runtime.takeCharacterListRetryRequest());
        CHECK_FALSE(runtime.takeCharacterListRetryRequest());
    }
    SECTION("another endpoint cannot inherit the pending hint") {
        REQUIRE(runtime.begin(f.root,"stock.example",3724,12340,f.executable));
        CHECK_FALSE(runtime.takeCharacterListRetryRequest());
    }
    SECTION("reset cannot leave a pending hint") {
        runtime.reset(); CHECK_FALSE(runtime.takeCharacterListRetryRequest());
    }
}

TEST_CASE("EZWoW patch metadata is bounded and cannot supply filesystem paths", "[protocol]") {
    using wowee::game::parseEzWowPatches;
    const std::vector<uint8_t> body(std::begin(ezPatchFixture),std::end(ezPatchFixture));
    const auto patches=parseEzWowPatches(body);
    REQUIRE(patches.size()==33);
    CHECK(patches.front().name=="01_dbc_diff.MPQ"); CHECK(patches.front().size==11182348);
    CHECK(patches.front().flags==2); CHECK(patches.front().checksum[0]==0x90);
    CHECK(patches.front().fastChecksum[0]==0x68); CHECK(patches.back().name=="99_models_attach53.MPQ");
    for (size_t n=0;n<body.size();++n) CHECK_THROWS(parseEzWowPatches(std::span<const uint8_t>(body).first(n)));
    auto changed=body; changed.push_back(0); CHECK_THROWS(parseEzWowPatches(changed));
    changed=body; std::fill_n(changed.begin()+5,4,0xFF); CHECK_THROWS(parseEzWowPatches(changed));
    changed=body; changed[9]='/'; CHECK_THROWS(parseEzWowPatches(changed));
    changed=body; changed[9]='.'; changed[10]='.'; CHECK_THROWS(parseEzWowPatches(changed));
    changed=body; changed[9]=0; CHECK_THROWS(parseEzWowPatches(changed));
    changed=body; std::fill_n(changed.begin()+24,4,0); CHECK_THROWS(parseEzWowPatches(changed));
    CHECK(parseEzWowPatches(std::vector<uint8_t>{1,0,0,0,0}).empty());
}

TEST_CASE("EZWoW known titles use their own bounded framing", "[protocol]") {
    using wowee::game::parseEzWowKnownTitles;
    const std::vector<uint8_t> body{3,0,0,0,3,2,0,0x8c,0,0x8d,0};
    const auto update = parseEzWowKnownTitles(body);
    CHECK(update.operation == 3); CHECK(update.bits == std::vector<uint16_t>{140,141});
    for (size_t n=0; n<body.size(); ++n)
        CHECK_THROWS(parseEzWowKnownTitles(std::span<const uint8_t>(body).first(n)));
    for (uint8_t operation : {0,4,255}) {
        auto invalid=body; invalid[4]=operation; CHECK_THROWS(parseEzWowKnownTitles(invalid));
    }
    auto trailing=body; trailing.push_back(0); CHECK_THROWS(parseEzWowKnownTitles(trailing));
    auto overflow=body; overflow[5]=overflow[6]=255; CHECK_THROWS(parseEzWowKnownTitles(overflow));
    for (uint8_t operation : {1,2,3}) {
        const auto empty=parseEzWowKnownTitles(std::vector<uint8_t>{3,0,0,0,operation,0,0});
        CHECK(empty.operation==operation); CHECK(empty.bits.empty());
    }
}
TEST_CASE("EZWoW forwards title operations only after loader setup without network acknowledgements", "[protocol]") {
    using namespace wowee::game;
    std::vector<KnownTitleUpdate> updates;
    std::vector<uint64_t> money;
    auto protocol=makeServerProtocol("ezwow", {"login.ezwow.org",3724,12340,
        {[&](const KnownTitleUpdate& update) { updates.push_back(update); },
         [&](uint64_t copper) { money.push_back(copper); }}});
    std::vector<uint16_t> outgoing;
    const auto send=[&](uint16_t opcode,const std::vector<uint8_t>&) { outgoing.push_back(opcode); };
    const std::vector<uint8_t> titles{3,0,0,0,3,2,0,0x8c,0,0x8d,0};
    SECTION("before initialization") {
        CHECK(protocol->packet(0x41D,titles,send)==ProtocolResult::Error);
        CHECK(updates.empty()); CHECK(outgoing.empty());
    }
    SECTION("after initialization") {
        protocol->warden(initializeTiming(0x14E720));
        for(size_t i=27;i-- >0;) REQUIRE(protocol->packet(0x2E9,bootstrapWrite(i),send)==ProtocolResult::Handled);
        protocol->warden(initializeTiming(0x46AE20));
        const std::vector<uint8_t> loader(std::begin(ezLoaderFixture),std::end(ezLoaderFixture));
        REQUIRE(protocol->packet(0x41D,loader,send)==ProtocolResult::Handled);
        REQUIRE(protocol->packet(0x41D,titles,send)==ProtocolResult::Handled);
        REQUIRE(updates.size()==1); CHECK(updates[0].operation==3);
        CHECK(updates[0].bits==std::vector<uint16_t>{140,141});
        CHECK(outgoing==std::vector<uint16_t>{0x51F});
        for(uint8_t operation : {1,2}) {
            auto delta=titles;delta[4]=operation;
            CHECK(protocol->packet(0x41D,delta,send)==ProtocolResult::Handled);
            CHECK(updates.back().operation==operation);
        }
        const std::vector<uint8_t> balance{5,0,0,0,0,0,0,0xc0,0x16,0x0a,0x7a,0x41};
        CHECK(protocol->packet(0x41D,balance,send)==ProtocolResult::Handled);
        CHECK(money==std::vector<uint64_t>{27304300}); CHECK(outgoing.size()==1);
        auto invalid=titles;invalid.pop_back();
        CHECK(protocol->packet(0x41D,invalid,send)==ProtocolResult::Error);
        CHECK(updates.size()==3); CHECK(outgoing.size()==1);
    }
}

TEST_CASE("EZWoW money is a validated little endian double without integer overflow", "[protocol]") {
    using wowee::game::parseEzWowMoney;
    const auto packet=[](double copper) {
        std::vector<uint8_t> bytes{5,0,0,0};
        const auto bits=std::bit_cast<uint64_t>(copper);
        for (unsigned i=0; i<8; ++i) bytes.push_back(uint8_t(bits>>(i*8)));
        return bytes;
    };
    CHECK(parseEzWowMoney(packet(27304300.0))==27304300);
    CHECK(parseEzWowMoney(packet(0.0))==0);
    CHECK(parseEzWowMoney(packet(4294967296.0))==4294967296ULL);
    CHECK(parseEzWowMoney(packet(9007199254740991.0))==9007199254740991ULL);
    for (double value : {-1.0,1.5,9007199254740992.0,
        std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        CHECK_THROWS(parseEzWowMoney(packet(value)));
    auto body=packet(27304300.0);
    for (size_t n=0; n<body.size(); ++n)
        CHECK_THROWS(parseEzWowMoney(std::span<const uint8_t>(body).first(n)));
    body.push_back(0); CHECK_THROWS(parseEzWowMoney(body));
    body=packet(0);body[0]=4;CHECK_THROWS(parseEzWowMoney(body));
}

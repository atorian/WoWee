// The pipeline cache kept between runs: handed back to the device that wrote
// it, refused for another device or driver, and never left half written.
#include <catch_amalgamated.hpp>

#include "rendering/pipeline_cache_file.hpp"

#include "test_support.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using wowee::rendering::loadPipelineCacheFile;
using wowee::rendering::PipelineCacheIdentity;
using wowee::rendering::savePipelineCacheFile;

namespace {

PipelineCacheIdentity device(uint32_t vendor, uint32_t id, uint8_t uuidByte) {
    PipelineCacheIdentity identity;
    identity.vendorId = vendor;
    identity.deviceId = id;
    identity.cacheUuid.fill(uuidByte);
    return identity;
}

/// What a driver hands back from vkGetPipelineCacheData: its header, then
/// whatever it keeps.
std::vector<char> cacheWrittenBy(const PipelineCacheIdentity& by, size_t payload = 64) {
    VkPipelineCacheHeaderVersionOne header{};
    header.headerSize = sizeof(header);
    header.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    header.vendorID = by.vendorId;
    header.deviceID = by.deviceId;
    std::memcpy(header.pipelineCacheUUID, by.cacheUuid.data(), VK_UUID_SIZE);
    std::vector<char> data(sizeof(header) + payload, 'p');
    std::memcpy(data.data(), &header, sizeof(header));
    return data;
}

} // namespace

TEST_CASE("a saved pipeline cache comes back to the device that wrote it", "[pipeline-cache]") {
    const auto dir = wowee::test::scratchDir("wowee_pipeline_cache_same");
    const auto path = dir / "cache" / "pipeline_cache.bin";
    const auto nvidia = device(0x10DE, 0x2684, 7);
    const auto saved = cacheWrittenBy(nvidia);

    REQUIRE(savePipelineCacheFile(path, saved));
    CHECK(loadPipelineCacheFile(path, nvidia) == saved);
    // Nothing left beside it from the write.
    CHECK_FALSE(std::filesystem::exists(dir / "cache" / "pipeline_cache.bin.partial"));
}

TEST_CASE("a pipeline cache from another driver or device is not handed over", "[pipeline-cache]") {
    const auto dir = wowee::test::scratchDir("wowee_pipeline_cache_other");
    const auto path = dir / "pipeline_cache.bin";
    const auto before = device(0x10DE, 0x2684, 7);
    REQUIRE(savePipelineCacheFile(path, cacheWrittenBy(before)));

    // A driver update changes the cache UUID.
    CHECK(loadPipelineCacheFile(path, device(0x10DE, 0x2684, 8)).empty());
    // Another card.
    CHECK(loadPipelineCacheFile(path, device(0x1002, 0x744C, 7)).empty());
}

TEST_CASE("a cut-off pipeline cache is not handed over", "[pipeline-cache]") {
    const auto dir = wowee::test::scratchDir("wowee_pipeline_cache_short");
    const auto path = dir / "pipeline_cache.bin";
    const auto gpu = device(0x8086, 0x56A0, 3);
    auto data = cacheWrittenBy(gpu);
    data.resize(10);
    REQUIRE(savePipelineCacheFile(path, data));
    CHECK(loadPipelineCacheFile(path, gpu).empty());

    CHECK(loadPipelineCacheFile(dir / "missing.bin", gpu).empty());
}

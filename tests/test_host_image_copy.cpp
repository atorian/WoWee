// When a texture goes up by host image copy. The usage narrows the memory an
// image may live in; where it leaves only host-visible memory, the texture
// would be sampled from system memory, or crowd the small window of video
// memory the CPU can see, so it takes the staging path instead.
#include <catch_amalgamated.hpp>

#include "rendering/host_image_copy.hpp"

using wowee::rendering::hostCopyKeepsDeviceMemory;

namespace {
constexpr uint32_t bit(uint32_t type) { return 1u << type; }
} // namespace

TEST_CASE("host copy is refused where it moves a texture out of video memory", "[texture]") {
    // An AMD card without resizable BAR: type 0 video memory, type 1 system
    // memory, type 2 the 256 MB window of video memory the CPU can map.
    const uint32_t deviceLocal = bit(0) | bit(2);
    const uint32_t without = bit(0) | bit(1) | bit(2);
    const uint32_t withHostTransfer = bit(1) | bit(2);
    CHECK_FALSE(hostCopyKeepsDeviceMemory(withHostTransfer, without, deviceLocal));
}

TEST_CASE("host copy is used where every memory type stays open to it", "[texture]") {
    // Unified memory: every type is device-local and host-visible.
    const uint32_t all = bit(0) | bit(1) | bit(2);
    CHECK(hostCopyKeepsDeviceMemory(all, all, all));

    // A discrete card whose driver writes video memory from the host itself.
    const uint32_t deviceLocal = bit(0);
    const uint32_t types = bit(0) | bit(1);
    CHECK(hostCopyKeepsDeviceMemory(types, types, deviceLocal));
}

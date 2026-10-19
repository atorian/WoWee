// Small uniform buffers as slots of a few large ones (rendering/uniform_slot_pool.hpp).
// A slot is what a material's descriptor points at and what its parameters
// are written through: two live slots must never share bytes, a freed one is
// taken again before another block is made, and every offset is one the
// device accepts for a uniform buffer.
#include <catch_amalgamated.hpp>

#include "rendering/uniform_slot_pool.hpp"

#include <cstring>
#include <memory>
#include <set>
#include <vector>

using wowee::rendering::UniformSlotPool;

namespace {

/// Blocks in host memory, standing in for the device's mapped buffers.
struct FakeDevice {
    std::vector<std::unique_ptr<std::vector<uint8_t>>> memory;
    size_t made = 0;
    size_t freed = 0;
    bool failing = false;

    UniformSlotPool pool(VkDeviceSize slotBytes, VkDeviceSize alignment, uint32_t perBlock) {
        return UniformSlotPool(
            slotBytes, alignment, perBlock, "test materials",
            [this](VkDeviceSize bytes) -> std::optional<UniformSlotPool::Block> {
                if (failing) return std::nullopt;
                memory.push_back(std::make_unique<std::vector<uint8_t>>(bytes));
                ++made;
                UniformSlotPool::Block block;
                block.buffer = reinterpret_cast<::VkBuffer>(static_cast<uintptr_t>(made));
                block.mapped = memory.back()->data();
                return block;
            },
            [this](const UniformSlotPool::Block&) { ++freed; });
    }
};

}  // namespace

TEST_CASE("live slots never share bytes, across blocks", "[rendering][uniform-slots]") {
    FakeDevice device;
    auto pool = device.pool(40, 64, 4);
    std::vector<UniformSlotPool::Slot> slots;
    for (int i = 0; i < 10; ++i) {
        slots.push_back(pool.allocate());
        REQUIRE(slots.back());
        std::memset(slots.back().mapped, i + 1, 40);
    }
    // Each still reads what was written through it.
    for (int i = 0; i < 10; ++i) {
        const auto* bytes = static_cast<const uint8_t*>(slots[i].mapped);
        for (int b = 0; b < 40; ++b) REQUIRE(bytes[b] == i + 1);
    }
    std::set<std::pair<::VkBuffer, VkDeviceSize>> places;
    for (const auto& s : slots) places.insert({s.buffer, s.offset});
    CHECK(places.size() == slots.size());
    CHECK(device.made == 3);   // 10 slots of 4 a block
}

TEST_CASE("every offset is aligned for a uniform descriptor", "[rendering][uniform-slots]") {
    FakeDevice device;
    auto pool = device.pool(200, 256, 8);
    for (int i = 0; i < 8; ++i) {
        const auto slot = pool.allocate();
        CHECK(slot.offset % 256 == 0);
        // The whole of the slot's bytes lie inside its block.
        CHECK(slot.offset + 200 <= 256 * 8);
    }
}

TEST_CASE("a freed slot is taken again before another block is made", "[rendering][uniform-slots]") {
    FakeDevice device;
    auto pool = device.pool(64, 64, 2);
    const auto a = pool.allocate();
    const auto b = pool.allocate();
    CHECK(device.made == 1);
    pool.free(a);
    const auto c = pool.allocate();
    CHECK(device.made == 1);
    CHECK(c.buffer == a.buffer);
    CHECK(c.offset == a.offset);
    CHECK(pool.slotsInUse() == 2);
    (void)b;
}

TEST_CASE("a block that cannot be made gives no slot, and the pool goes on", "[rendering][uniform-slots]") {
    FakeDevice device;
    auto pool = device.pool(64, 64, 1);
    REQUIRE(pool.allocate());
    device.failing = true;
    CHECK_FALSE(pool.allocate());
    device.failing = false;
    CHECK(pool.allocate());
}

TEST_CASE("released, the pool lets its blocks go and ignores late frees", "[rendering][uniform-slots]") {
    FakeDevice device;
    auto pool = device.pool(64, 64, 2);
    const auto a = pool.allocate();
    (void)pool.allocate();
    (void)pool.allocate();
    pool.release();
    CHECK(device.freed == 2);
    pool.free(a);   // a deferred deletion arriving after shutdown
    CHECK(pool.slotsInUse() == 0);
    CHECK_FALSE(pool.allocate());
}

// The slots the copies of a scene address its instances by: an instance keeps
// its slot while others come and go, a freed slot goes to the next instance
// added, and a copy catching up reads each changed instance once and nothing
// that did not change.
#include <catch_amalgamated.hpp>

#include "rendering/instance_slots.hpp"

#include <algorithm>
#include <vector>

using wowee::rendering::InstanceSlots;

namespace {

/// The (slot, id) pairs a catch-up visits, sorted by id.
std::vector<std::pair<uint32_t, uint32_t>> caughtUp(InstanceSlots& slots) {
    std::vector<std::pair<uint32_t, uint32_t>> visited;
    slots.catchUp(InstanceSlots::Mirror::RayTracing,
                  [&](uint32_t slot, uint32_t id) { visited.emplace_back(slot, id); });
    std::sort(visited.begin(), visited.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return visited;
}

} // namespace

TEST_CASE("an instance keeps its slot while others come and go", "[instance slots]") {
    InstanceSlots slots;
    for (uint32_t id = 1; id <= 5; ++id) slots.add(id, 7);
    const uint32_t first = slots.slotOf(1), last = slots.slotOf(5);
    slots.remove(2);
    slots.remove(4);
    CHECK(slots.slotOf(1) == first);
    CHECK(slots.slotOf(5) == last);
    CHECK(slots.idAt(last) == 5);
    CHECK(slots.slotOf(2) == InstanceSlots::invalid);
    CHECK(slots.instanceCount() == 3);
}

TEST_CASE("a freed slot goes to the next instance, which is read as new", "[instance slots]") {
    InstanceSlots slots;
    slots.add(1, 7);
    const uint32_t freed = slots.add(2, 7);
    (void)caughtUp(slots);
    slots.remove(2);
    CHECK(slots.idAt(freed) == InstanceSlots::invalid);
    const uint32_t reused = slots.add(3, 8);
    CHECK(reused == freed);
    CHECK(slots.slotCount() == 2);  // no row grows for it
    const auto visited = caughtUp(slots);
    REQUIRE(visited.size() == 1);
    CHECK(visited[0] == std::pair<uint32_t, uint32_t>{freed, 3});
}

TEST_CASE("a catch-up reads each changed instance once, and nothing in a scene that stood still",
          "[instance slots]") {
    InstanceSlots slots;
    for (uint32_t id = 1; id <= 1000; ++id) slots.add(id, id % 5);
    CHECK(caughtUp(slots).size() == 1000);
    CHECK(caughtUp(slots).empty());
    for (int i = 0; i < 3; ++i) slots.changed(42);
    slots.changed(77);
    const auto visited = caughtUp(slots);
    REQUIRE(visited.size() == 2);
    CHECK(visited[0].second == 42);
    CHECK(visited[1].second == 77);
    CHECK(caughtUp(slots).empty());
}

TEST_CASE("an instance removed after it changed is not read", "[instance slots]") {
    InstanceSlots slots;
    for (uint32_t id = 1; id <= 4; ++id) slots.add(id, 1);
    (void)caughtUp(slots);
    slots.changed(1);
    slots.changed(2);
    slots.changed(3);
    slots.remove(2);
    const auto visited = caughtUp(slots);
    REQUIRE(visited.size() == 2);
    CHECK(visited[0].second == 1);
    CHECK(visited[1].second == 3);
}

TEST_CASE("a model's change wakes its instances and no others", "[instance slots]") {
    InstanceSlots slots;
    slots.add(1, 10);
    slots.add(2, 11);
    slots.add(3, 10);
    (void)caughtUp(slots);
    slots.remove(1);  // its model's list keeps the rest
    slots.modelChanged(10);
    const auto visited = caughtUp(slots);
    REQUIRE(visited.size() == 1);
    CHECK(visited[0].second == 3);
}

TEST_CASE("a cleared scene has no instances and nothing to catch up on", "[instance slots]") {
    InstanceSlots slots;
    slots.add(1, 10);
    slots.add(2, 10);
    slots.clear();
    CHECK(slots.instanceCount() == 0);
    CHECK(slots.slotOf(1) == InstanceSlots::invalid);
    CHECK(caughtUp(slots).empty());
    CHECK(slots.add(3, 10) == 0);
}

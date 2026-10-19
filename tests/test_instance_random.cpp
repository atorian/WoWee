// The random numbers a model instance draws (rendering/instance_random.hpp):
// what one instance draws depends on that instance alone, so instances can be
// updated on any thread in any order and each still behaves the same.
#include <catch_amalgamated.hpp>

#include "rendering/instance_random.hpp"

#include <array>
#include <random>
#include <vector>

using wowee::rendering::InstanceRandom;

TEST_CASE("an instance draws the same numbers however other instances are interleaved",
          "[rendering][instances]") {
    // Alone.
    InstanceRandom alone(42);
    std::vector<float> expected;
    for (int i = 0; i < 64; ++i) expected.push_back(alone.uniform(0.0f, 1.0f));

    // Between two others drawing heavily, as on a shared worker.
    InstanceRandom mine(42), other1(7), other2(43);
    std::vector<float> got;
    for (int i = 0; i < 64; ++i) {
        for (int k = 0; k < i % 5; ++k) (void)other1();
        got.push_back(mine.uniform(0.0f, 1.0f));
        for (int k = 0; k < 3; ++k) (void)other2.below(17);
    }
    CHECK(got == expected);
}

TEST_CASE("neighbouring ids draw different numbers", "[rendering][instances]") {
    // Instance ids are consecutive; a seed that only shifted would leave a
    // courtyard of torches flickering in step.
    InstanceRandom a(1000), b(1001);
    int same = 0;
    for (int i = 0; i < 32; ++i) same += a() == b();
    CHECK(same == 0);
}

TEST_CASE("draws land in their ranges and cover them", "[rendering][instances]") {
    InstanceRandom r(5);
    std::array<int, 10> buckets{};
    for (int i = 0; i < 20000; ++i) {
        const float u = r.uniform(-1.0f, 1.0f);
        REQUIRE(u >= -1.0f);
        REQUIRE(u < 1.0f);
        const uint32_t k = r.below(10);
        REQUIRE(k < 10);
        ++buckets[k];
    }
    for (int count : buckets) CHECK(count > 1700);   // 2000 expected per bucket
    CHECK(r.below(0) == 0);

    // The standard distributions take it, as the particle code uses them.
    std::uniform_real_distribution<float> dist(2.0f, 3.0f);
    const float d = dist(r);
    CHECK(d >= 2.0f);
    CHECK(d < 3.0f);
}

#include <catch_amalgamated.hpp>
#include "rendering/frame_pool.hpp"
#include "core/frame_worker.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <vector>

namespace {
std::atomic<size_t> payloadAllocations{0};
template<class T> struct CountedAllocator {
    using value_type = T;
    CountedAllocator() = default;
    template<class U> CountedAllocator(const CountedAllocator<U>&) {}
    T* allocate(size_t n) { ++payloadAllocations; return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, size_t n) { std::allocator<T>{}.deallocate(p, n); }
    bool operator==(const CountedAllocator&) const { return true; }
};
struct Frame {
    std::vector<int, CountedAllocator<int>> draws;
    std::shared_ptr<int> resource;
    void clear() noexcept { draws.clear(); resource.reset(); }
};
using Pool = wowee::rendering::FramePool<Frame>;
}

TEST_CASE("released frame payload storage serves subsequent frames without allocating", "[frame-pool]") {
    Pool pool;
    { auto frame = pool.acquire(); frame->draws.assign(1024, 7); }
    const auto warmed = payloadAllocations.load();
    for (int value = 0; value < 20; ++value) {
        auto frame = pool.acquire();
        REQUIRE(frame->draws.empty());
        frame->draws.assign(1024, value);
    }
    CHECK(payloadAllocations.load() == warmed);
}

TEST_CASE("recording retains an immutable frame while the producer fills later frames", "[frame-pool]") {
    Pool pool;
    wowee::core::FrameWorker worker(true);
    auto first = pool.acquire();
    first->draws.assign(200, 17);
    first->resource = std::make_shared<int>(42);
    std::weak_ptr<int> resource = first->resource;
    std::promise<void> entered, release;
    auto gate = release.get_future();
    bool unchanged = false;
    worker.submit(worker.reserve(), [frame = std::shared_ptr<const Frame>(std::move(first)), &entered, &gate, &unchanged] {
        entered.set_value();
        gate.wait();
        unchanged = frame->draws.size() == 200 && frame->draws.front() == 17 &&
                    frame->draws.back() == 17 && *frame->resource == 42;
    });
    entered.get_future().wait();
    for (int i = 0; i < 10; ++i) {
        auto next = pool.acquire();
        next->draws.assign(400, i);
    }
    CHECK_FALSE(resource.expired());
    release.set_value();
    worker.drain();
    CHECK(unchanged);
    CHECK(resource.expired()); // Idle capacity must not pin model/texture ownership.
}

TEST_CASE("a retained consumer and a discarded extraction do not release each other's resources", "[frame-pool]") {
    Pool pool;
    auto producer = pool.acquire();
    producer->draws.assign(30, 9);
    producer->resource = std::make_shared<int>(5);
    std::weak_ptr<int> resource = producer->resource;
    std::shared_ptr<const Frame> consumer = producer;
    producer.reset();
    { auto discarded = pool.acquire(); discarded->draws.assign(80, 3); }
    CHECK(consumer->draws.size() == 30);
    CHECK(consumer->draws.back() == 9);
    CHECK_FALSE(resource.expired());
    consumer.reset();
    CHECK(resource.expired());
    auto next = pool.acquire();
    CHECK(next->draws.empty());
    CHECK_FALSE(next->resource);
}

TEST_CASE("a frame remains readable after its pool owner is destroyed", "[frame-pool]") {
    std::shared_ptr<const Frame> held;
    std::weak_ptr<int> resource;
    {
        Pool pool;
        auto frame = pool.acquire();
        frame->draws.assign(12, 8);
        frame->resource = std::make_shared<int>(19);
        resource = frame->resource;
        held = std::move(frame);
    }
    CHECK(held->draws.front() == 8);
    CHECK(*held->resource == 19);
    held.reset();
    CHECK(resource.expired());
}

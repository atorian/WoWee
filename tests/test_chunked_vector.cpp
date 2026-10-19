// The M2 renderer's instance list (rendering/chunked_vector.hpp): growing it
// moves nothing already in it, and it still does what the list was used for -
// index, swap-remove, walk in order.
#include <catch_amalgamated.hpp>

#include "rendering/chunked_vector.hpp"

#include <memory>
#include <string>
#include <vector>

using wowee::rendering::ChunkedVector;

namespace {

/// Counts how often it is moved or copied: an element that moves while the
/// list grows is the spike this container exists to remove.
struct Tracked {
    static inline int relocations = 0;
    int value = 0;
    explicit Tracked(int v) : value(v) {}
    Tracked(Tracked&& o) noexcept : value(o.value) { ++relocations; }
    Tracked& operator=(Tracked&& o) noexcept {
        value = o.value;
        ++relocations;
        return *this;
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
};

}  // namespace

TEST_CASE("growing the list moves none of the elements already in it", "[chunked_vector]") {
    ChunkedVector<Tracked, 8> list;
    for (int i = 0; i < 5; ++i) list.emplace_back(i);
    const Tracked* first = &list[0];
    const Tracked* fifth = &list[4];
    Tracked::relocations = 0;

    for (int i = 5; i < 1000; ++i) list.emplace_back(i);

    CHECK(Tracked::relocations == 0);
    CHECK(&list[0] == first);
    CHECK(&list[4] == fifth);
    CHECK(list.size() == 1000);
    CHECK(list[999].value == 999);
}

TEST_CASE("a swap-remove fills the hole from the end, across chunks", "[chunked_vector]") {
    ChunkedVector<std::string, 4> list;
    for (int i = 0; i < 10; ++i) list.push_back("item" + std::to_string(i));

    // Remove index 2 the way the renderer does: the last into the hole.
    list[2] = std::move(list.back());
    list.pop_back();

    REQUIRE(list.size() == 9);
    CHECK(list[2] == "item9");
    CHECK(list.back() == "item8");

    std::vector<std::string> walked;
    for (const auto& s : list) walked.push_back(s);
    CHECK(walked == std::vector<std::string>{"item0", "item1", "item9", "item3", "item4",
                                             "item5", "item6", "item7", "item8"});
}

TEST_CASE("emptied down and refilled, the list holds what was put back", "[chunked_vector]") {
    ChunkedVector<std::unique_ptr<int>, 4> list;
    for (int i = 0; i < 9; ++i) list.push_back(std::make_unique<int>(i));
    while (!list.empty()) list.pop_back();
    CHECK(list.size() == 0);

    for (int i = 0; i < 6; ++i) list.push_back(std::make_unique<int>(100 + i));
    CHECK(*list.front() == 100);
    CHECK(*list.back() == 105);

    list.clear();
    CHECK(list.empty());
    CHECK(list.begin() == list.end());
}

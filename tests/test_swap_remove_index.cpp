// Lists of indices into an array kept right across swap-removes
// (rendering/swap_remove_index.hpp): after any sequence of removals each
// list names exactly the elements a rebuild from scratch would.
#include <catch_amalgamated.hpp>

#include "rendering/swap_remove_index.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using wowee::rendering::swapRemoveIndex;

namespace {

struct Element {
    uint32_t id = 0;
    bool animated = false;
    bool emits = false;
};

/// What a full rebuild lists: the indices of the elements with the property.
std::vector<size_t> rebuilt(const std::vector<Element>& elements, bool Element::*property) {
    std::vector<size_t> out;
    for (size_t i = 0; i < elements.size(); ++i) {
        if (elements[i].*property) out.push_back(i);
    }
    return out;
}

std::vector<size_t> sorted(std::vector<size_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

TEST_CASE("index lists follow every swap-remove as a rebuild would", "[rendering][instances]") {
    std::mt19937 rng(1234);
    std::vector<Element> elements(500);
    for (size_t i = 0; i < elements.size(); ++i) {
        elements[i] = {static_cast<uint32_t>(i), rng() % 3 == 0, rng() % 5 == 0};
    }
    std::vector<size_t> animated = rebuilt(elements, &Element::animated);
    std::vector<size_t> emitting = rebuilt(elements, &Element::emits);

    while (!elements.empty()) {
        // Remove the first, the last, or one in between: each moves differently.
        const size_t pick = rng() % 3;
        const size_t idx = pick == 0 ? 0 : pick == 1 ? elements.size() - 1 : rng() % elements.size();
        const size_t last = elements.size() - 1;
        if (idx < last) elements[idx] = elements.back();
        elements.pop_back();
        swapRemoveIndex(animated, idx, last);
        swapRemoveIndex(emitting, idx, last);

        REQUIRE(sorted(animated) == rebuilt(elements, &Element::animated));
        REQUIRE(sorted(emitting) == rebuilt(elements, &Element::emits));
    }
}

TEST_CASE("removing an element the list does not name leaves it alone but for the move",
          "[rendering][instances]") {
    std::vector<size_t> list{1, 4, 9};
    // Element 2 goes; element 9, the last, moves into slot 2.
    swapRemoveIndex(list, 2, 9);
    CHECK(sorted(list) == std::vector<size_t>{1, 2, 4});
}

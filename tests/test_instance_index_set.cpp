// Sets of indices into an array of instances kept right across swap-removes
// (rendering/instance_index_set.hpp): after any sequence of removals, batch
// removals and instances hidden and shown again, each set names exactly the
// instances a rebuild from scratch would.
#include <catch_amalgamated.hpp>

#include "rendering/instance_index_set.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

using wowee::rendering::InstanceIndexSet;

namespace {

struct Element {
    uint32_t id = 0;
    bool animated = false;
    bool emits = false;
    bool hidden = false;
};

/// What a full rebuild lists: the indices of the shown elements with the property.
std::vector<size_t> rebuilt(const std::vector<Element>& elements, bool Element::*property) {
    std::vector<size_t> out;
    for (size_t i = 0; i < elements.size(); ++i) {
        if (!elements[i].hidden && elements[i].*property) out.push_back(i);
    }
    return out;
}

std::vector<size_t> sorted(const InstanceIndexSet& set) {
    std::vector<size_t> v(set.begin(), set.end());
    std::sort(v.begin(), v.end());
    return v;
}

struct World {
    std::vector<Element> elements;
    InstanceIndexSet animated, emitting;

    void file(size_t i) {
        if (elements[i].hidden) return;
        if (elements[i].animated) animated.insert(i);
        if (elements[i].emits) emitting.insert(i);
    }
    /// Takes out the element at idx the way the renderer does: the last
    /// one moves into its place.
    void remove(size_t idx) {
        animated.erase(idx);
        emitting.erase(idx);
        const size_t last = elements.size() - 1;
        if (idx != last) {
            elements[idx] = elements.back();
            animated.move(last, idx);
            emitting.move(last, idx);
        }
        elements.pop_back();
    }
    void check() const {
        REQUIRE(sorted(animated) == rebuilt(elements, &Element::animated));
        REQUIRE(sorted(emitting) == rebuilt(elements, &Element::emits));
    }
};

} // namespace

TEST_CASE("index sets follow every swap-remove as a rebuild would", "[rendering][instances]") {
    std::mt19937 rng(1234);
    World w;
    w.elements.resize(500);
    for (size_t i = 0; i < w.elements.size(); ++i) {
        w.elements[i] = {static_cast<uint32_t>(i), rng() % 3 == 0, rng() % 5 == 0};
        w.file(i);
    }
    while (!w.elements.empty()) {
        // Remove the first, the last, or one in between: each moves differently.
        const size_t pick = rng() % 3;
        const size_t idx = pick == 0 ? 0 : pick == 1 ? w.elements.size() - 1 : rng() % w.elements.size();
        w.remove(idx);
        w.check();
    }
}

TEST_CASE("a tile's instances leaving together, highest first, leave the sets right",
          "[rendering][instances]") {
    // As M2Renderer::removeInstances takes out a tile's doodads: every hole
    // filled from the back, the highest first.
    std::mt19937 rng(99);
    World w;
    w.elements.resize(2000);
    for (size_t i = 0; i < w.elements.size(); ++i) {
        w.elements[i] = {static_cast<uint32_t>(i), rng() % 2 == 0, rng() % 4 == 0};
        w.file(i);
    }
    for (int round = 0; round < 8; ++round) {
        std::vector<size_t> holes;
        for (size_t i = 0; i < w.elements.size(); ++i) {
            if (rng() % 7 == 0) holes.push_back(i);
        }
        std::sort(holes.begin(), holes.end(), std::greater<>());
        for (size_t hole : holes) w.remove(hole);
        w.check();
        // New ones arrive at the end.
        for (int k = 0; k < 100; ++k) {
            w.elements.push_back({static_cast<uint32_t>(10000 + round * 100 + k), rng() % 2 == 0, rng() % 3 == 0});
            w.file(w.elements.size() - 1);
        }
        w.check();
    }
}

TEST_CASE("hidden instances leave the sets and come back to them", "[rendering][instances]") {
    std::mt19937 rng(7);
    World w;
    w.elements.resize(300);
    for (size_t i = 0; i < w.elements.size(); ++i) {
        w.elements[i] = {static_cast<uint32_t>(i), true, rng() % 2 == 0};
        w.file(i);
    }
    for (int step = 0; step < 600; ++step) {
        const size_t i = rng() % w.elements.size();
        if (step % 5 == 4) {
            w.remove(i);
        } else if (w.elements[i].hidden) {
            w.elements[i].hidden = false;
            w.file(i);
        } else {
            w.elements[i].hidden = true;
            w.animated.erase(i);
            w.emitting.erase(i);
        }
        w.check();
        if (w.elements.empty()) break;
    }
}

// The queue the scene's changes wait in until the frame's apply point
// (rendering/deferred_calls.hpp, under rendering/scene_commands.hpp): a change
// asked for is made only when the queue is applied, in the order asked; one
// asked for while applying is made in the same apply; and the changes of a
// world that is left are never made in the next.
#include <catch_amalgamated.hpp>

#include "rendering/deferred_calls.hpp"

#include <string>
#include <vector>

using wowee::rendering::DeferredCalls;

namespace {

/// Stands in for the renderers: what was done to it, in order.
struct Scene {
    std::vector<std::string> done;
};

} // namespace

TEST_CASE("changes are made only at the apply point, in the order asked", "[rendering][scene-commands]") {
    DeferredCalls<Scene> calls;
    Scene scene;
    calls.post([](Scene& s) { s.done.push_back("create 7"); });
    calls.post([](Scene& s) { s.done.push_back("play stand on 7"); });
    calls.post([](Scene& s) { s.done.push_back("remove 7"); });
    CHECK(scene.done.empty());
    CHECK(calls.size() == 3);

    CHECK(calls.apply(scene) == 3);
    CHECK(scene.done == std::vector<std::string>{"create 7", "play stand on 7", "remove 7"});
    CHECK(calls.empty());

    // Applied again with nothing asked, nothing more happens.
    CHECK(calls.apply(scene) == 0);
    CHECK(scene.done.size() == 3);
}

TEST_CASE("a change asked for while applying is made in the same apply, after the rest",
          "[rendering][scene-commands]") {
    DeferredCalls<Scene> calls;
    Scene scene;
    calls.post([&calls](Scene& s) {
        s.done.push_back("attach weapon");
        // Making one change asks for another, as a weapon brings its enchant.
        calls.post([](Scene& inner) { inner.done.push_back("attach enchant"); });
    });
    calls.post([](Scene& s) { s.done.push_back("move"); });

    CHECK(calls.apply(scene) == 3);
    CHECK(scene.done == std::vector<std::string>{"attach weapon", "move", "attach enchant"});
    CHECK(calls.empty());
}

TEST_CASE("the changes of a world that is left are never made", "[rendering][scene-commands]") {
    DeferredCalls<Scene> calls;
    Scene scene;
    calls.post([](Scene& s) { s.done.push_back("create on the old map"); });
    calls.post([](Scene& s) { s.done.push_back("animate on the old map"); });
    CHECK(calls.discard() == 2);

    calls.post([](Scene& s) { s.done.push_back("create on the new map"); });
    CHECK(calls.apply(scene) == 1);
    CHECK(scene.done == std::vector<std::string>{"create on the new map"});
}

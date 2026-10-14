// A GameObject's animation for its state, as the client picks it
// (GameObject_C.cpp, WoW.exe 3.3.5a 0x70d600 / 0x70d690 / 0x70d1e0).
#include <catch_amalgamated.hpp>

#include "rendering/game_object_animation.hpp"

#include <set>

using namespace wowee::rendering;
using End = M2AnimationPlay::End;

namespace {

HasSequence having(std::set<uint32_t> ids) {
    return [ids = std::move(ids)](uint32_t id) { return ids.count(id) > 0; };
}

} // namespace

TEST_CASE("Dalaran's fountain shows Tirion while ACTIVE and the crystal while READY", "[gameobject][anim]") {
    // Stand, Closed and Opened, no transitions.
    const auto has = having({go_anim::kStand, go_anim::kClosed, go_anim::kOpened});
    const auto active = goAnimationOnSpawn(GoState::Active, has);
    CHECK(active.now.animationId == go_anim::kOpened);
    CHECK(active.now.end == End::Loop);
    CHECK_FALSE(active.next.has_value());
    CHECK(goAnimationOnSpawn(GoState::Ready, has).now.animationId == go_anim::kClosed);
    // Opening in view with no Open: straight to Opened.
    CHECK(goAnimationOnChange(GoState::Ready, GoState::Active, has).now.animationId == go_anim::kOpened);
}

TEST_CASE("A lamp lights through Open and then holds Opened", "[gameobject][anim]") {
    const auto has = having({go_anim::kStand, go_anim::kClosed, go_anim::kOpen, go_anim::kOpened});
    const auto lighting = goAnimationOnChange(GoState::Ready, GoState::Active, has);
    CHECK(lighting.now.animationId == go_anim::kOpen);
    CHECK(lighting.now.end == End::Next);
    REQUIRE(lighting.next.has_value());
    CHECK(lighting.next->animationId == go_anim::kOpened);
    // Put out with no Close: Open played backwards, then Closed.
    const auto out = goAnimationOnChange(GoState::Active, GoState::Ready, has);
    CHECK(out.now.animationId == go_anim::kOpen);
    CHECK(out.now.reversed);
    REQUIRE(out.next.has_value());
    CHECK(out.next->animationId == go_anim::kClosed);
}

TEST_CASE("Something seen already open shows no opening", "[gameobject][anim]") {
    const auto has = having({go_anim::kStand, go_anim::kOpen, go_anim::kClose});
    // No Opened: the opening held at its end.
    const auto open = goAnimationOnSpawn(GoState::Active, has);
    CHECK(open.now.animationId == go_anim::kOpen);
    CHECK(open.now.startAtEnd);
    CHECK(open.now.end == End::Hold);
    // No Closed: the closing held at its end.
    const auto shut = goAnimationOnSpawn(GoState::Ready, has);
    CHECK(shut.now.animationId == go_anim::kClose);
    CHECK(shut.now.startAtEnd);
}

TEST_CASE("A model with only Stand plays it in every state", "[gameobject][anim]") {
    // Undercity's lift doors: one sequence, Stand, which is the opening. The
    // fallback chain ends at Stand and drops its flags, open or shut.
    const auto has = having({go_anim::kStand});
    for (const GoState state : {GoState::Active, GoState::Ready}) {
        const auto plan = goAnimationOnSpawn(state, has);
        CHECK(plan.now.animationId == go_anim::kStand);
        CHECK(plan.now.end == End::Loop);
        CHECK_FALSE(plan.now.startAtEnd);
        CHECK_FALSE(plan.now.reversed);
    }
}

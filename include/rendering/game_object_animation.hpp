#pragma once

// Which animation a GameObject's model plays for its state, as the client
// picks it (GameObject_C.cpp in WoW.exe 3.3.5a: state table 0xada938, on
// creation 0x70d600, on a state change 0x70d690, when a transition ends
// 0x70d7e0, a missing sequence 0x70d1e0; render spec section 6d).
//
// Map doodads always play Stand. A GameObject plays Closed while READY and
// Opened while ACTIVE, going between them through Open and Close: Dalaran's
// centre fountain is the crystal while READY and Tirion's statue while
// ACTIVE, its lamps unlit and lit.

#include <cstdint>
#include <functional>
#include <optional>

namespace wowee::rendering {

/// AnimationData ids of a GameObject's states.
namespace go_anim {
constexpr uint32_t kStand = 0;
constexpr uint32_t kSpawn = 145;
constexpr uint32_t kClose = 146;
constexpr uint32_t kClosed = 147;
constexpr uint32_t kOpen = 148;
constexpr uint32_t kOpened = 149;
constexpr uint32_t kDestroy = 150;
constexpr uint32_t kDestroyed = 151;
constexpr uint32_t kRebuild = 152;
} // namespace go_anim

/// GO state, GAMEOBJECT_BYTES_1 byte 0.
enum class GoState : uint8_t {
    Active = 0,             ///< open, lit, on
    Ready = 1,              ///< closed, the default
    ActiveAlternative = 2,  ///< destroyed
};

/// One animation to play on a model.
struct M2AnimationPlay {
    uint32_t animationId = 0;
    bool reversed = false;     ///< from its end back to its start
    bool frozen = false;       ///< held still at its first frame in the direction of play
    enum class End : uint8_t {
        Loop,   ///< starts over - unless its sequence plays once (flag 0x01)
        Hold,   ///< stays on its last frame
        Next,   ///< goes on to the plan's next animation
    } end = End::Loop;
    /// Starts on its last frame: a resting pose that is the end of a
    /// transition, shown without playing the transition.
    bool startAtEnd = false;
};

/// What to play now, and what follows it when it is a one-shot transition.
struct GoAnimationPlan {
    M2AnimationPlay now;
    std::optional<M2AnimationPlay> next;
};

using HasSequence = std::function<bool(uint32_t animationId)>;

namespace detail {

// A state's resting animation, or what stands in for it (0x70d1e0).
inline M2AnimationPlay resting(uint32_t wanted, const HasSequence& has) {
    using End = M2AnimationPlay::End;
    if (has(wanted)) return {.animationId = wanted, .end = End::Loop};
    switch (wanted) {
        case go_anim::kOpened:
            // The opening held at its end, or the closing held at its start.
            if (has(go_anim::kOpen)) return {.animationId = go_anim::kOpen, .end = End::Hold, .startAtEnd = true};
            if (has(go_anim::kClose)) return {.animationId = go_anim::kClose, .frozen = true};
            if (has(go_anim::kDestroyed)) return {.animationId = go_anim::kDestroyed, .end = End::Loop};
            // The fallback chain runs out and ends at Stand, played as it is:
            // the flags met on the way are dropped (0x826350). A door with
            // one sequence, which is its opening, plays it on.
            return {.animationId = go_anim::kStand, .end = End::Loop};
        case go_anim::kClosed:
            if (has(go_anim::kClose)) return {.animationId = go_anim::kClose, .end = End::Hold, .startAtEnd = true};
            if (has(go_anim::kOpen)) return {.animationId = go_anim::kOpen, .frozen = true};
            return {.animationId = go_anim::kStand, .end = End::Loop};
        default:
            return {.animationId = go_anim::kStand, .end = End::Loop};
    }
}

// A one-shot transition into a state, or what stands in for it.
inline GoAnimationPlan transition(uint32_t wanted, uint32_t restingAfter, const HasSequence& has) {
    using End = M2AnimationPlay::End;
    const M2AnimationPlay after = resting(restingAfter, has);
    if (has(wanted)) return {.now = {.animationId = wanted, .end = End::Next}, .next = after};
    if (wanted == go_anim::kOpen) {
        if (has(go_anim::kClose))
            return {.now = {.animationId = go_anim::kClose, .reversed = true, .end = End::Next}, .next = after};
        if (has(go_anim::kDestroy))
            return {.now = {.animationId = go_anim::kDestroy, .end = End::Next}, .next = after};
    } else if (wanted == go_anim::kClose) {
        if (has(go_anim::kOpen))
            return {.now = {.animationId = go_anim::kOpen, .reversed = true, .end = End::Next}, .next = after};
    }
    return {.now = after};
}

} // namespace detail

/// The animation an object shows when it comes into view: its state's
/// resting one, with no transition - one that spawns ACTIVE is simply open.
inline GoAnimationPlan goAnimationOnSpawn(GoState state, const HasSequence& has) {
    switch (state) {
        case GoState::Active: return {.now = detail::resting(go_anim::kOpened, has)};
        case GoState::ActiveAlternative: return {.now = detail::resting(go_anim::kDestroyed, has)};
        case GoState::Ready:
        default: return {.now = detail::resting(go_anim::kClosed, has)};
    }
}

/// The animation for a change of state while in view: the transition, then
/// the new state's resting animation.
inline GoAnimationPlan goAnimationOnChange(GoState from, GoState to, const HasSequence& has) {
    switch (to) {
        case GoState::Active:
            if (from == GoState::Ready) return detail::transition(go_anim::kOpen, go_anim::kOpened, has);
            return {.now = detail::resting(go_anim::kOpened, has)};
        case GoState::ActiveAlternative:
            if (from != GoState::ActiveAlternative)
                return detail::transition(go_anim::kDestroy, go_anim::kDestroyed, has);
            return {.now = detail::resting(go_anim::kDestroyed, has)};
        case GoState::Ready:
        default:
            if (from == GoState::Active) return detail::transition(go_anim::kClose, go_anim::kClosed, has);
            if (from == GoState::ActiveAlternative)
                return detail::transition(go_anim::kRebuild, go_anim::kClosed, has);
            return {.now = detail::resting(go_anim::kClosed, has)};
    }
}

} // namespace wowee::rendering

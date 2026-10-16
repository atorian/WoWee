#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace wowee {
namespace rendering { class CameraController; }

namespace core {

class CharacterSession;

/// The keys, text, wheel and focus changes the Input phase reads for the
/// interface, held until the Interface phase delivers them.
///
/// Lua runs only in Interface, so what the platform says in Input waits here:
/// a key's press runs its binding, its release goes back to the binding the
/// press ran, an edit box takes typed text, the wheel scrolls a frame or runs
/// its binding. The same dispatch whatever read the platform - SDL's queue in
/// the client, a test's own queue in ClientHarness.
class InterfaceInput {
public:
    /// Whether the Input phase keeps this event for the interface.
    [[nodiscard]] static bool takes(const SDL_Event& event);

    /// Input phase: keep an event for the Interface phase. Text is copied: an
    /// SDL text event points at storage that does not outlive the poll.
    void push(const SDL_Event& event);

    /// Interface phase: deliver everything kept since the last delivery, in
    /// order, to the character's interface while it hears the world.
    ///
    /// `camera` zooms on the wheel when no interface takes it. A key nothing
    /// in the interface wanted, and that the client's own screens are not
    /// typing into, goes to `unclaimedKey` - the client's development keys.
    void dispatch(CharacterSession* session, rendering::CameraController* camera,
                  const std::function<void(const SDL_Event&)>& unclaimedKey);

private:
    struct Kept {
        SDL_Event event;
        std::string text;
    };
    std::vector<Kept> pending_;
    /// The part of a wheel turn that has not yet made a whole click of the
    /// wheel's binding - a trackpad reports fractions.
    float wheelBindingRemainder_ = 0.0f;
    /// Whether shift going down has been said in the log yet; see dispatch.
    bool saidShift_ = false;
};

} // namespace core
} // namespace wowee

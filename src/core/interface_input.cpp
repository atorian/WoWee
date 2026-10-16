#include "core/interface_input.hpp"

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/character_session.hpp"
#include "core/logger.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/keybinding_manager.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdint>

namespace wowee::core {

namespace {

/// A wheel event's travel in clicks of a clicking wheel, which is what the
/// interface scrolls by.
///
/// macOS reports a trackpad or a Magic Mouse in pixels, and SDL passes those
/// on at a tenth - a "line" per ten pixels - while a clicking wheel comes
/// through as whole clicks. A two-finger swipe with its momentum runs to a
/// hundred of those lines, and a scroll frame moves half a page per notch, so
/// even at a quarter of the scroll speed one swipe went through the quest log
/// ten pages at a time. Elsewhere a trackpad already reports in clicks.
///
/// A precise delta is one with a fraction, which a click never has: SDL rounds
/// those away from zero. Pixels that happen to come to a whole ten do not have
/// one either, so a gesture stays precise for half a second after its last
/// fraction, and momentum keeps it going well past that.
float wheelClicks(float delta, uint64_t timestampNs) {
#if defined(__APPLE__)
    constexpr float kLinesPerClick = 20.0f;
    constexpr uint64_t kGestureGapNs = 500'000'000ull;
    static uint64_t lastPreciseNs = 0;
    const bool fractional = delta != std::trunc(delta);
    if (fractional) lastPreciseNs = timestampNs;
    const bool precise = fractional ||
        (lastPreciseNs != 0 && timestampNs - lastPreciseNs < kGestureGapNs);
    return precise ? delta / kLinesPerClick : delta;
#else
    (void)timestampNs;
    return delta;
#endif
}

/// WoW's name for a mouse button a key binding can be given to: the middle
/// button and the two side buttons. The left and right buttons are the
/// camera's and the interface's, and are not bound on their own.
std::string mouseBindingName(uint8_t sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_MIDDLE: return "BUTTON3";
        case SDL_BUTTON_X1:     return "BUTTON4";
        case SDL_BUTTON_X2:     return "BUTTON5";
        default:                return {};
    }
}

/// The modifiers held now, as the binding tables read them.
struct HeldModifiers {
    bool shift = false, ctrl = false, alt = false;
};
HeldModifiers heldModifiers() {
    const SDL_Keymod mods = SDL_GetModState();
    return {(mods & SDL_KMOD_SHIFT) != 0, (mods & SDL_KMOD_CTRL) != 0, (mods & SDL_KMOD_ALT) != 0};
}

} // namespace

bool InterfaceInput::takes(const SDL_Event& event) {
    // Focus lost too: the keys held when it went will send no release, and
    // letting go of their bindings runs Lua, which waits for the Interface
    // phase.
    // And the mouse buttons a key binding can name - BUTTON3 to BUTTON5 -
    // which run their bindings there too.
    const bool bindableMouseButton =
        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        !mouseBindingName(event.button.button).empty();
    return event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN ||
           event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_WHEEL ||
           event.type == SDL_EVENT_WINDOW_FOCUS_LOST || bindableMouseButton;
}

void InterfaceInput::push(const SDL_Event& event) {
    pending_.push_back({event, event.type == SDL_EVENT_TEXT_INPUT && event.text.text
                                   ? event.text.text : ""});
}

void InterfaceInput::dispatch(CharacterSession* session, rendering::CameraController* camera,
                              const std::function<void(const SDL_Event&)>& unclaimedKey) {
    // The character's interface in any state, and the same only while it
    // hears the world - what input goes to.
    const auto characterAddons = [session]() -> addons::AddonManager* {
        return session ? session->addonManager() : nullptr;
    };
    const auto worldInterface = [session]() -> addons::AddonManager* {
        return session && session->interfaceHoldsWorld() ? session->addonManager() : nullptr;
    };
    auto input = std::move(pending_);
    pending_.clear();
    for (auto& saved : input) {
        SDL_Event event = saved.event;
        if (event.type == SDL_EVENT_TEXT_INPUT) event.text.text = saved.text.c_str();
        // A key coming up goes back to the binding its press ran, before
        // anything else looks at it and whatever has the keyboard now: the
        // press of W walked, and an edit box opened since does not stop the
        // release from stopping. A key whose press ran no binding has nothing
        // to release, and passes on as before.
        if (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            if (auto* addons = worldInterface()) {
                if (auto* engine = addons->getLuaEngine()) {
                    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                        engine->releaseBindingKeys();
                    } else {
                        engine->dispatchBindingKey(addons::bindingKeyName(event.key.key),
                                                   false, false, false, false);
                    }
                }
            }
        }
        // The middle and side mouse buttons are keys to the bindings, as in
        // the original client: an account that puts action buttons on BUTTON4
        // and BUTTON5 casts with them. The middle button is a frame's when it
        // is over one that takes the mouse; the side buttons no frame takes.
        // The release goes back like a key's.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            auto* addons = worldInterface();
            auto* engine = addons ? addons->getLuaEngine() : nullptr;
            const std::string name = mouseBindingName(event.button.button);
            if (engine && !name.empty()) {
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    engine->dispatchBindingKey(name, false, false, false, false);
                } else if (!ImGui::GetIO().WantCaptureMouse &&
                           !(event.button.button == SDL_BUTTON_MIDDLE && engine->mouseOverFrameXml())) {
                    const HeldModifiers mods = heldModifiers();
                    engine->dispatchBindingKey(name, mods.shift, mods.ctrl, mods.alt, true);
                }
            }
        }
        if (camera && !ImGui::GetIO().WantCaptureMouse) {
            if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                // The interface gets first refusal, and only where a
                // frame under the cursor asked for the wheel. Zooming
                // the camera while scrolling a quest log is what
                // happens without the check, and a frame that did not
                // ask must not swallow it either.
                bool takenByUi = false;
                const float clicks = wheelClicks(event.wheel.y, event.wheel.timestamp);
                if (auto* addons = characterAddons()) {
                    // ImGui's cursor, not SDL's, because that is what
                    // the rest of the widget dispatch is fed and the
                    // two need not agree on a scaled display. Flipped
                    // to bottom-origin for the same reason the button
                    // dispatch is: the tree measures upward.
                    const ImGuiIO& mio = ImGui::GetIO();
                    takenByUi = addons->getLuaEngine()->dispatchMouseWheel(
                        mio.MousePos.x, mio.DisplaySize.y - mio.MousePos.y, clicks);
                }
                auto* world = worldInterface();
                auto* engine = world ? world->getLuaEngine() : nullptr;
                if (!takenByUi && engine) {
                    // Then the wheel's binding, one press and release a
                    // click: CAMERAZOOMIN and CAMERAZOOMOUT by default, an
                    // action button on an account that moved zoom to Page Up.
                    // A trackpad's fractions carry over to the next event.
                    wheelBindingRemainder_ += clicks;
                    const HeldModifiers mods = heldModifiers();
                    while (std::abs(wheelBindingRemainder_) >= 1.0f) {
                        const bool up = wheelBindingRemainder_ > 0.0f;
                        wheelBindingRemainder_ += up ? -1.0f : 1.0f;
                        const std::string name = up ? "MOUSEWHEELUP" : "MOUSEWHEELDOWN";
                        engine->dispatchBindingKey(name, mods.shift, mods.ctrl, mods.alt, true);
                        engine->dispatchBindingKey(name, false, false, false, false);
                    }
                } else if (!takenByUi) {
                    // No interface to ask: the camera's own zoom.
                    camera->processMouseWheel(static_cast<float>(event.wheel.y));
                }
            }
        }
        // Typed text, when an edit box is listening for it - or, when none
        // is, whichever frame has asked for the keyboard.
        //
        // A dialog reads its digits from OnChar and not from OnKeyDown:
        // StackSplitFrame's key handler passes numbers straight through on
        // purpose, so the amount could only be reached with the arrows and
        // typing "12" did nothing at all.
        if (event.type == SDL_EVENT_TEXT_INPUT) {
            if (auto* addons = worldInterface()) {
                if (auto* engine = addons->getLuaEngine()) {
                    if (engine->editBoxHasFocus()) {
                        // Unless it is the slash that opened this box,
                        // arriving after it took focus - see
                        // noteChatOpenedBySlash.
                        if (!ui::consumeChatSlashEcho(event.text.text)) {
                            engine->dispatchText(event.text.text);
                        }
                    } else {
                        engine->dispatchFrameChar(event.text.text);
                    }
                }
            }
        }
        // Debug controls
        else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            // Shift, control and alt announce themselves. The interface
            // watches these to swap what a tooltip shows and what a
            // click will do - item comparison appears on shift, and an
            // action button's self-cast indicator on alt. Four frames
            // listen and none had ever been told.
            if (auto* addons = worldInterface()) {
                const char* modName = nullptr;
                switch (event.key.key) {
                    case SDLK_LSHIFT: modName = "LSHIFT"; break;
                    case SDLK_RSHIFT: modName = "RSHIFT"; break;
                    case SDLK_LCTRL:  modName = "LCTRL";  break;
                    case SDLK_RCTRL:  modName = "RCTRL";  break;
                    case SDLK_LALT:   modName = "LALT";   break;
                    case SDLK_RALT:   modName = "RALT";   break;
                    default: break;
                }
                // Repeats are not changes: holding shift sends a stream
                // of key-downs and the interface would rebuild every
                // tooltip in the game for each one.
                if (modName && event.key.repeat == 0) {
                    // Said once for shift going down, upstream of any
                    // tooltip. IsModifiedClick(COMPAREITEMS) answered false
                    // through two runs in which the key was pressed, which
                    // is either a key this client never sees or a question
                    // nothing asks again once the tooltip is already up.
                    // This separates them: a line here and no matching
                    // true from the gate means the event arrives and
                    // nothing acts on it.
                    if (!saidShift_ && event.type == SDL_EVENT_KEY_DOWN &&
                        (modName[1] == 'S')) {
                        saidShift_ = true;
                        LOG_WARNING("MODIFIER_STATE_CHANGED fired for ",
                                    modName, " down - the client sees shift");
                    }
                    addons->fireEvent(
                        "MODIFIER_STATE_CHANGED",
                        {modName, event.type == SDL_EVENT_KEY_DOWN ? "1" : "0"});
                }
            }
        }
        if (event.type == SDL_EVENT_KEY_DOWN) {
            // An addon's edit box takes the keystroke before anything
            // else looks at it. Otherwise typing into one would also
            // walk the character, and backspace would trip a keybind.
            if (auto* addons = worldInterface()) {
                if (auto* engine = addons->getLuaEngine();
                    engine && engine->editBoxHasFocus()) {
                    // Command counts as control here, because this is the
                    // copy-and-paste modifier and on a Mac that is the one
                    // every other program uses. Windows and Linux send
                    // control; both are taken, so the gesture is the one
                    // the player already knows on whichever they are on.
                    const bool ctrl =
                        (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
                    // Said before the dispatch, because dispatching is
                    // what lets go of the focus that the check above
                    // just used - ask afterwards and the box no longer
                    // admits to having taken anything.
                    //
                    // Only the three that let go. Every other key
                    // leaves the box focused, so the poll's own typing
                    // guard still answers for them.
                    switch (event.key.key) {
                        case SDLK_ESCAPE:
                            ui::noteInterfaceConsumedKey(ImGuiKey_Escape);
                            // The first of the three ways a press ends
                            // early, and the correct one: an edit box
                            // had the keyboard, so Escape closed the
                            // box and means nothing further. WoW stops
                            // here too.
                            LOG_WARNING("Escape: taken in the pump by a "
                                        "focused edit box; it closes the "
                                        "box and stops");
                            break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            ui::noteInterfaceConsumedKey(ImGuiKey_Enter);
                            break;
                        case SDLK_TAB:
                            ui::noteInterfaceConsumedKey(ImGuiKey_Tab);
                            break;
                        default: break;
                    }
                    engine->dispatchKey(event.key.key, ctrl);
                    continue;
                }
                // No edit box, but a dialog may still be listening -
                // the colour picker, the stack splitter, the coin
                // pickup. The key is only swallowed when one of them
                // actually takes it, so with nothing up the movement
                // keys and every binding carry on exactly as before.
                if (auto* engine = addons->getLuaEngine()) {
                    if (engine->dispatchFrameKey(event.key.key, true)) {
                        // Escape says so, because "Escape does nothing"
                        // is a live report and this is one of the three
                        // ways the press can end before the chain that
                        // decides what it means ever runs. At warning:
                        // the default log carries nothing else, so an
                        // info line here is a line nobody will ever
                        // see. See the pair in GameScreen.
                        if (event.key.key == SDLK_ESCAPE) {
                            LOG_WARNING("Escape: taken in the pump by a "
                                        "frame listening for keys; the "
                                        "chain below never runs");
                        }
                        continue;
                    }
                }
                // Then whatever the interface has bound to the key.
                //
                // Last of the three, because a focused edit box and an
                // open dialog both outrank a binding - which is WoW's
                // order too. It declines for anything this client
                // performs itself, so the keys that already work are
                // untouched; what it adds is every command the client
                // has no path for, which until now could be bound in
                // the interface's own key-binding panel and then never
                // honoured by any press. Movement and the action bar are
                // bindings like any other, so a key the account moves them
                // to is the key that walks or casts.
                //
                // Not while one of this client's own text fields is typed
                // into: the keystroke is a letter there.
                if (auto* engine = addons->getLuaEngine();
                    engine && !ImGui::GetIO().WantTextInput) {
                    const SDL_Keymod mods = event.key.mod;
                    if (engine->dispatchBindingKey(
                            addons::bindingKeyName(event.key.key),
                            (mods & SDL_KMOD_SHIFT) != 0,
                            (mods & SDL_KMOD_CTRL) != 0,
                            (mods & SDL_KMOD_ALT) != 0, true)) {
                        // The fourth way a press can end in the pump - an
                        // interface key binding claimed it - which had no
                        // line. For the DEFAULT Escape this does not fire:
                        // Escape binds to TOGGLEGAMEMENU, which
                        // clientActsOnBinding lists, so dispatchBindingKey
                        // yields (returns false) and the poll chain below
                        // runs. It fires only if Escape has been rebound to
                        // a FrameXML command a binding script handles - in
                        // which case *that* is why the game menu never
                        // opens, and this line names it. So it is a real
                        // signal for a rebound Escape, not the default one.
                        if (event.key.key == SDLK_ESCAPE) {
                            LOG_WARNING("Escape: taken in the pump by an "
                                        "interface key binding (rebound off "
                                        "TOGGLEGAMEMENU); the game-menu chain "
                                        "never runs");
                        }
                        continue;
                    }
                }
            }
            // The third way, and the ordinary one: nothing in the
            // interface wanted it, so the poll further down decides.
            // Said as well as the other two, because the whole value of
            // these lines is that exactly one of them appears per
            // press - silence would mean the key never arrived at all,
            // and that is a different fault in a different place.
            if (event.key.key == SDLK_ESCAPE) {
                LOG_WARNING("Escape: through the pump untaken; the chain "
                            "below decides");
            }
            // Skip non-function-key input when UI (chat) has keyboard focus
            bool uiHasKeyboard = ImGui::GetIO().WantCaptureKeyboard ||
                                 ui::interfaceTakingTypedInput();
            auto sc = event.key.scancode;
            bool isFKey = (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12);
            if (uiHasKeyboard && !isFKey) {
                continue;  // Let ImGui handle the keystroke
            }
            if (unclaimedKey) unclaimedKey(event);
        }
    }
}

} // namespace wowee::core

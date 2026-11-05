#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <glm/glm.hpp>

namespace wowee {
namespace core {

class Input {
public:
    static Input& getInstance();

    void update();

    // Keyboard
    [[nodiscard]] bool isKeyPressed(SDL_Scancode key) const;

    /// Holds a key down from somewhere that is not a keyboard: a pad button
    /// standing in for one of the few keys the client still reads by
    /// scancode - Tab, Print Screen, the shift the left bumper holds.
    /// Movement is not one of them; the sticks hold movement commands on the
    /// camera instead. Merged over the hardware state, so a keyboard still
    /// works alongside.
    void setVirtualKey(SDL_Scancode key, bool held);

    /// Holds a mouse button down from somewhere that is not a mouse.
    ///
    /// The controller's pointer is the reason, and it is the same argument as
    /// the virtual keys above: targeting, looting and every click on the world
    /// is a chain that starts at isMouseButtonJustPressed, and a pad has no
    /// mouse. The pointer itself is the real one - it is warped, so everything
    /// that asks where it is gets the truth - and only the button is faked.
    void setVirtualMouseButton(int button, bool held);
    [[nodiscard]] bool isKeyJustPressed(SDL_Scancode key) const;

    // Mouse
    [[nodiscard]] bool isMouseButtonPressed(int button) const;
    [[nodiscard]] bool isMouseButtonJustPressed(int button) const;
    [[nodiscard]] bool isMouseButtonJustReleased(int button) const;

    [[nodiscard]] glm::vec2 getMousePosition() const { return mousePosition; }
    [[nodiscard]] glm::vec2 getMouseDelta() const { return mouseDelta; }

    [[nodiscard]] bool isMouseLocked() const { return mouseLocked; }

private:
    Input() = default;
    ~Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    static constexpr int NUM_KEYS = SDL_SCANCODE_COUNT;
    static constexpr int NUM_MOUSE_BUTTONS = 8;

    std::array<bool, NUM_KEYS> currentKeyState = {};
    std::array<bool, NUM_KEYS> virtualKeyState = {};
    std::array<bool, NUM_KEYS> previousKeyState = {};

    std::array<bool, NUM_MOUSE_BUTTONS> currentMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> virtualMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> previousMouseState = {};

    glm::vec2 mousePosition = glm::vec2(0.0f);
    glm::vec2 previousMousePosition = glm::vec2(0.0f);
    glm::vec2 mouseDelta = glm::vec2(0.0f);
    bool mouseLocked = false;
};

} // namespace core
} // namespace wowee

#include "ui/agent_controls.hpp"

#include "rendering/camera_controller.hpp"

#include <imgui.h>

#include <algorithm>

namespace wowee {
namespace ui {

AgentControls::Frame AgentControls::update(float deltaTime, rendering::CameraController* camera) {
    const Frame frame = advance(deltaTime);
    if (!camera) return frame;
    // applyLookDelta takes mouse pixels times the mouse's sensitivity, and
    // turns right for a positive x; dividing the sensitivity out keeps a
    // request in degrees whatever the player set it to.
    const float sensitivity = std::max(camera->getMouseSensitivity(), 0.0001f);
    const float yaw = frame.turnDegrees + frame.lookYawDegrees;
    if (yaw != 0.0f || frame.lookPitchDegrees != 0.0f)
        camera->applyLookDelta(yaw / sensitivity, -frame.lookPitchDegrees / sensitivity);
    if (frame.zoomNotches != 0.0f) camera->processMouseWheel(frame.zoomNotches);
    return frame;
}

void AgentControls::drawIndicator() const {
    if (!control_) return;
    const char* text = "Agent has control - press a key, click or Ctrl+Shift+H to take it back";
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 at((display.x - size.x) * 0.5f, 12.0f);
    draw->AddRectFilled(ImVec2(at.x - 10.0f, at.y - 6.0f), ImVec2(at.x + size.x + 10.0f, at.y + size.y + 6.0f),
                        IM_COL32(120, 20, 20, 210), 6.0f);
    draw->AddText(at, IM_COL32(255, 230, 120, 255), text);
}

} // namespace ui
} // namespace wowee

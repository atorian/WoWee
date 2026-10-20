#pragma once

namespace wowee::rendering {

/// Requested on the game side, copied with FrameInputs, applied between
/// frames by the owner of the post-process resources.
struct FramePostProcessSettings {
    bool fxaa = false;
    bool fsr = false;
    bool fsr2 = false;
    bool framegen = false;
    float scale = 1.0f;
    float sharpness = 1.6f;
    float jitterSign = 0.38f;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    float brightness = 1.0f;
    float intoxication = 0.0f;
    bool operator==(const FramePostProcessSettings&) const = default;
};

} // namespace wowee::rendering

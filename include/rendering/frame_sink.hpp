#pragma once

#include <cstdint>
#include <functional>

namespace wowee {
namespace rendering {

/// Where the renderer hands the frames it reads back from the GPU while the
/// screen is being recorded. The renderer does the reading back; what is made
/// of the frames - an encoder, with the sound - belongs to whoever attached it.
class FrameSink {
public:
    /// One frame, in memory the sink reads and then gives back through
    /// release - from its own thread, so release must be safe there.
    struct Frame {
        const uint8_t* bgra = nullptr;
        uint32_t stride = 0;   ///< bytes from one row to the next
        int64_t pts = 0;       ///< in frames at the sink's rate, from the start
        std::function<void()> release;
    };

    virtual ~FrameSink() = default;

    /// The frame due now, if one is: its pts goes in *pts.
    virtual bool frameDue(int64_t* pts) const = 0;
    /// That frame was captured and will be submitted when the GPU is done.
    virtual void frameTaken(int64_t pts) = 0;
    /// That frame could not be captured - every readback buffer was still out.
    virtual void frameDropped() = 0;
    /// Hand over a captured frame. Never blocks.
    virtual void submit(Frame frame) = 0;
    /// How long it has been taking frames, for the indicator on screen.
    [[nodiscard]] virtual double elapsedSeconds() const = 0;
};

} // namespace rendering
} // namespace wowee

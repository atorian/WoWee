#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

/// Frames the CPU may record ahead of the GPU. A buffer the CPU writes every
/// frame needs one copy per frame in flight: the GPU can still be reading the
/// previous frame's while this one's is written.
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

} // namespace rendering
} // namespace wowee

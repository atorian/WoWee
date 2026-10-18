#pragma once

/// The WMO header bits and batch runs read beyond the loader - by the
/// renderer's lighting (rendering/wmo_lighting.hpp) and by collision - kept
/// apart from wmo_loader.hpp so a header can name them without the loader.

#include <cstdint>

namespace wowee::pipeline {

/// MOGP flag 0x2000: the group is indoors. It is what separates a building's
/// inside from the porch and the roof, which are groups of the same model.
inline constexpr uint32_t kWMOGroupIndoor = 0x2000;

/// MOGP flag 0x4: the group has vertex colours (MOCV).
inline constexpr uint32_t kWMOGroupHasVertexColors = 0x4;

/// MOHD flag 0x2: the model is drawn by the client's unified path, which
/// lights a batch by its group rather than by its run - Dalaran, Stormwind;
/// most older buildings do not have it.
inline constexpr uint16_t kWMOModelUnifiedRenderPath = 0x2;

/// Which of a group's three runs of batches a batch is in. MOBA lists the
/// transition batches first, then the interior ones, then the exterior ones,
/// and the MOGP header says how many of each there are. WoW.exe 12340 picks a
/// batch's light by the run it is in (rendering/wmo_lighting.hpp).
enum class WMOBatchKind : uint8_t {
    /// Where the inside meets the outside - a doorway, the frame of a window:
    /// drawn lit and unlit at once, weighed by the vertex colours' alpha.
    Transition,
    Interior,
    Exterior,
};

}  // namespace wowee::pipeline

#pragma once

/// Which light each batch of a WMO group is drawn with.
///
/// WoW.exe 12340 does not light a group as a whole. Its draw loops pick a
/// lighting mode for every MOBA batch (CMapObj 0x7a8b10 sets it):
///
///   0  none - the vertex colours are the light;
///   1  the zone's ambient and sun;
///   2  the window light, a second set for MOMT F_WINDOW (0x20) materials;
///   3  the building's MOHD ambient and no sun.
///
/// Which one depends on the model's path and the batch's run (WMOBatchKind):
///
/// - A unified model (MOHD 0x2 - Dalaran, Stormwind; draw 0x7a9380) lights a
///   batch by its group: in an interior group (MOGP & 0x48 == 0) by the
///   window light for a window and mode 3 otherwise, in an exterior group by
///   the sun unless the material is F_UNLIT.
/// - An older model whose group has vertex colours (0x7ac9f0) lights a batch
///   by its run: an interior batch by its vertex colours alone, an exterior
///   batch by the sun - or the window light - whatever its group's flags.
///   The window of a building of this kind, Duskwood's inn among them, is
///   the one exterior batch of an interior group.
/// - An older model's group without vertex colours (0x7ac6a0) lights every
///   batch by the sun unless it is F_UNLIT.
///
/// A transition batch - the first transBatchCount of a group with vertex
/// colours, a doorway or the frame round a window - is drawn twice by both
/// paths, lit and then unlit, the two weighed by the vertex colours' alpha.
/// The lit half is the sun (the window light for a window, nothing for an
/// F_UNLIT one on the unified path); the unlit half is mode 3 on the unified
/// path and mode 0 on the older one.
///
/// The night glow of an F_SIDN material is in the lit shader every mode but 0
/// uses, so mode 3 glows too and mode 0 never does (wmo_night_glow.hpp).

#include <cstdint>

#include "pipeline/wmo_flags.hpp"

namespace wowee::rendering {

/// The client's lighting modes. The values are what wmo.frag reads.
enum class WmoLight : int32_t {
    VertexColour = 0,
    Sun = 1,
    Window = 2,
    BuildingAmbient = 3,
};

/// How one batch is lit: by `light`, or - for a transition batch - by `light`
/// where the vertex colour's alpha is 1 and by `under` where it is 0.
struct WmoBatchLighting {
    WmoLight light = WmoLight::Sun;
    bool transition = false;
    WmoLight under = WmoLight::VertexColour;  ///< meaningful only for a transition

    bool operator==(const WmoBatchLighting&) const = default;
};

inline constexpr uint32_t kWmoMaterialUnlit = 0x1;   ///< MOMT F_UNLIT
inline constexpr uint32_t kWmoMaterialWindow = 0x20; ///< MOMT F_WINDOW

/// The light the client draws a batch with.
///
/// `modelFlags` are the MOHD flags, `groupFlags` the MOGP flags (0x4 is the
/// vertex colours, 0x8 and 0x40 exterior), `materialFlags` the MOMT flags of
/// the batch's material.
inline WmoBatchLighting wmoBatchLighting(uint16_t modelFlags, uint32_t groupFlags,
                                         pipeline::WMOBatchKind kind, uint32_t materialFlags) {
    const bool unlit = (materialFlags & kWmoMaterialUnlit) != 0;
    const bool window = (materialFlags & kWmoMaterialWindow) != 0;
    const bool vertexColours = (groupFlags & pipeline::kWMOGroupHasVertexColors) != 0;
    const WmoLight sunOrWindow = window ? WmoLight::Window : WmoLight::Sun;

    if ((modelFlags & pipeline::kWMOModelUnifiedRenderPath) != 0) {
        if (kind == pipeline::WMOBatchKind::Transition) {
            return {.light = unlit ? WmoLight::VertexColour : sunOrWindow,
                    .transition = true, .under = WmoLight::BuildingAmbient};
        }
        if ((groupFlags & (0x8u | 0x40u)) == 0) {
            return {.light = window ? WmoLight::Window : WmoLight::BuildingAmbient};
        }
        return {.light = unlit ? WmoLight::VertexColour : WmoLight::Sun};
    }

    if (!vertexColours) return {.light = unlit ? WmoLight::VertexColour : WmoLight::Sun};
    switch (kind) {
    case pipeline::WMOBatchKind::Transition:
        return {.light = sunOrWindow, .transition = true, .under = WmoLight::VertexColour};
    case pipeline::WMOBatchKind::Interior:
        return {.light = WmoLight::VertexColour};
    case pipeline::WMOBatchKind::Exterior:
        break;
    }
    return {.light = sunOrWindow};
}

/// Whether the sun's shadow map falls on a group's batches: on every batch of
/// an exterior group (MOGP & 0x48), lit or not, and on nothing indoors.
///
/// WoW.exe 12340 marks each group of a building interior or exterior by those
/// two flags alone (0x7bdfb5) and binds an interior group only the units'
/// shadow map, on every batch of it: its window and doorway batches too,
/// which wmoBatchLighting lights by the sun or the window light without the
/// sun's map shading them. So the trees outside cast no shadow into a room.
/// This took the sun's map onto a room's sunlit and window batches and onto
/// every batch of a group showing the sky outside (MOGP 0x100), and a tree's
/// shadow lay across the floor of rooms the client keeps clear.
inline bool wmoReceivesExteriorShadows(uint32_t groupFlags) {
    return (groupFlags & (0x8u | 0x40u)) != 0;
}

}  // namespace wowee::rendering

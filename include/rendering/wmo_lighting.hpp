#pragma once
#include <cstdint>

namespace wowee::rendering {
// MOGP's interior bit describes a portal group, not whether sunlight reaches
// every surface. Orgrimmar's streets are interior + ExteriorLit or ShowExteriorSky.
inline bool wmoUsesExteriorLighting(uint32_t groupFlags, uint32_t materialFlags) {
    return (groupFlags & (0x8u | 0x40u)) != 0 || (materialFlags & 0x8u) != 0;
}
inline bool wmoUsesBakedInteriorLighting(uint32_t groupFlags, uint32_t materialFlags) {
    return (groupFlags & 0x2000u) != 0 && !wmoUsesExteriorLighting(groupFlags, materialFlags);
}
inline bool wmoReceivesExteriorShadows(uint32_t groupFlags, bool exteriorLighting) {
    return exteriorLighting || (groupFlags & 0x100u) != 0;
}
} // namespace wowee::rendering
